/*
 * Copyright (C) 2026 Gary Hsieh <romanticamaj@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <archive.h>
#include <archive_entry.h>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <glibmm.h>

#include "pbd/basename.h"
#include "pbd/enumwriter.h"

#include "ardour/audioregion.h"
#include "ardour/audio_track.h"
#include "ardour/broadcast_info.h"
#include "ardour/export_channel_configuration.h"
#include "ardour/export_filename.h"
#include "ardour/export_format_specification.h"
#include "ardour/export_handler.h"
#include "ardour/export_status.h"
#include "ardour/export_timespan.h"
#include "ardour/file_source.h"
#include "ardour/import_status.h"
#include "ardour/midi_track.h"
#include "ardour/playlist.h"
#include "ardour/presentation_info.h"
#include "ardour/region.h"
#include "ardour/region_factory.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/source.h"
#include "ardour/track.h"
#include "ardour/utils.h"

#include "common.h"

using namespace ARDOUR;
using namespace SessionUtils;
namespace pt = boost::property_tree;
namespace fs = std::filesystem;

struct RenderSettings
{
	RenderSettings ()
		: sample_rate (0)
		, sample_format (ExportFormatBase::SF_16)
		, normalize (false)
		, broadcast (false)
	{}

	int                            sample_rate;
	ExportFormatBase::SampleFormat sample_format;
	bool                           normalize;
	bool                           broadcast;
};

struct RegionLocation
{
	std::shared_ptr<AudioTrack> track;
	std::shared_ptr<Playlist>   playlist;
	std::shared_ptr<Region>     region;
};

struct JournalEntry
{
	std::string entry_id;
	std::string op;
	std::string command_json;
	std::string status;
	std::string pre_hash;
	std::string post_hash;
	std::string touched_tracks;
	std::string touched_playlists;
	std::string touched_regions;
	std::string touched_sources;
	std::string error_message;
};

struct SnapshotInfo
{
	std::string path;
	std::string sha256;
	bool        captured = false;
};

static double
parse_time_number (std::string const& value)
{
	char*  end    = 0;
	double number = std::strtod (value.c_str (), &end);
	if (!end || *end != '\0') {
		throw std::runtime_error ("invalid time position: " + value);
	}
	return number;
}

static std::string
json_escape (std::string const& input)
{
	std::ostringstream out;
	for (std::string::const_iterator i = input.begin (); i != input.end (); ++i) {
		switch (*i) {
			case '"':
				out << "\\\"";
				break;
			case '\\':
				out << "\\\\";
				break;
			case '\b':
				out << "\\b";
				break;
			case '\f':
				out << "\\f";
				break;
			case '\n':
				out << "\\n";
				break;
			case '\r':
				out << "\\r";
				break;
			case '\t':
				out << "\\t";
				break;
			default:
				out << *i;
				break;
		}
	}
	return out.str ();
}

static std::string
ptree_json (pt::ptree const& tree)
{
	std::ostringstream out;
	pt::write_json (out, tree, false);
	std::string json = out.str ();
	while (!json.empty () && (json[json.size () - 1] == '\n' || json[json.size () - 1] == '\r')) {
		json.erase (json.size () - 1);
	}
	return json;
}

static std::string
utc_now_json ()
{
	std::time_t now = std::time (0);
	std::tm     tm;
#ifdef _WIN32
	gmtime_s (&tm, &now);
#else
	gmtime_r (&now, &tm);
#endif

	std::ostringstream out;
	out << std::put_time (&tm, "%Y-%m-%dT%H:%M:%SZ");
	return out.str ();
}

static std::string
sha256_json (std::string const& value)
{
	gchar* digest = g_compute_checksum_for_string (G_CHECKSUM_SHA256, value.c_str (), value.size ());
	if (!digest) {
		throw std::runtime_error ("failed to compute sha256");
	}
	std::string result = std::string ("sha256:") + digest;
	g_free (digest);
	return result;
}

static std::string
sha256_file (std::string const& path)
{
	GChecksum* checksum = g_checksum_new (G_CHECKSUM_SHA256);
	if (!checksum) {
		throw std::runtime_error ("failed to create sha256 checksum");
	}

	std::ifstream file (path.c_str (), std::ios::in | std::ios::binary);
	if (!file) {
		g_checksum_free (checksum);
		throw std::runtime_error ("cannot read file for sha256: " + path);
	}

	char buffer[16384];
	while (file) {
		file.read (buffer, sizeof (buffer));
		std::streamsize count = file.gcount ();
		if (count > 0) {
			g_checksum_update (checksum, reinterpret_cast<guchar const*> (buffer), count);
		}
	}

	std::string result = std::string ("sha256:") + g_checksum_get_string (checksum);
	g_checksum_free (checksum);
	return result;
}

static std::string
route_type (std::shared_ptr<Route> const& route)
{
	if (std::dynamic_pointer_cast<AudioTrack> (route)) {
		return "audio_track";
	}
	if (std::dynamic_pointer_cast<MidiTrack> (route)) {
		return "midi_track";
	}
	return "route";
}

static std::string
data_type_name (DataType const& type)
{
	if (type == DataType::AUDIO) {
		return "audio";
	}
	if (type == DataType::MIDI) {
		return "midi";
	}
	return "unknown";
}

static samplepos_t
parse_position (Session& session, std::string const& value)
{
	std::vector<double> parts;
	std::string         current;

	for (std::string::const_iterator i = value.begin (); i != value.end (); ++i) {
		if (*i == ':') {
			if (current.empty ()) {
				throw std::runtime_error ("invalid time position: " + value);
			}
			parts.push_back (parse_time_number (current));
			current.clear ();
		} else {
			current += *i;
		}
	}

	if (current.empty ()) {
		throw std::runtime_error ("invalid time position: " + value);
	}
	parts.push_back (parse_time_number (current));

	double seconds = 0.0;
	if (parts.size () == 1) {
		seconds = parts[0];
	} else if (parts.size () == 2) {
		seconds = (parts[0] * 60.0) + parts[1];
	} else if (parts.size () == 3) {
		seconds = (parts[0] * 3600.0) + (parts[1] * 60.0) + parts[2];
	} else {
		throw std::runtime_error ("invalid time position: " + value);
	}

	if (seconds < 0.0) {
		throw std::runtime_error ("time position cannot be negative: " + value);
	}

	return static_cast<samplepos_t> (std::llround (seconds * session.nominal_sample_rate ()));
}

static std::shared_ptr<AudioTrack>
find_audio_track (Session& session, std::string const& track_id, std::string const& track_name)
{
	std::shared_ptr<RouteList const> routes = session.get_routes ();
	if (!routes) {
		return std::shared_ptr<AudioTrack> ();
	}

	for (RouteList::const_iterator i = routes->begin (); i != routes->end (); ++i) {
		std::shared_ptr<AudioTrack> track = std::dynamic_pointer_cast<AudioTrack> (*i);
		if (!track) {
			continue;
		}
		if (!track_id.empty () && track->id ().to_s () == track_id) {
			return track;
		}
		if (track_id.empty () && !track_name.empty () && track->name () == track_name) {
			return track;
		}
	}

	return std::shared_ptr<AudioTrack> ();
}

static RegionLocation
find_region_location (Session& session, std::string const& region_id, std::string const& region_name)
{
	std::shared_ptr<RouteList const> routes = session.get_routes ();
	if (!routes) {
		return RegionLocation ();
	}

	for (RouteList::const_iterator i = routes->begin (); i != routes->end (); ++i) {
		std::shared_ptr<AudioTrack> track = std::dynamic_pointer_cast<AudioTrack> (*i);
		if (!track || !track->playlist ()) {
			continue;
		}

		std::shared_ptr<RegionList> regions = track->playlist ()->region_list ();
		for (RegionList::const_iterator r = regions->begin (); r != regions->end (); ++r) {
			if (!*r) {
				continue;
			}
			if (!region_id.empty () && (*r)->id ().to_s () == region_id) {
				RegionLocation location;
				location.track = track;
				location.playlist = track->playlist ();
				location.region = *r;
				return location;
			}
			if (region_id.empty () && !region_name.empty () && (*r)->name () == region_name) {
				RegionLocation location;
				location.track = track;
				location.playlist = track->playlist ();
				location.region = *r;
				return location;
			}
		}
	}

	return RegionLocation ();
}

static std::string
unique_region_name (std::string const& initial_name)
{
	std::string name = initial_name;
	while (RegionFactory::region_by_name (name)) {
		name = bump_name_once (name, '.');
	}
	return name;
}

static std::string
sample_format_name (ExportFormatBase::SampleFormat sample_format)
{
	return enum_2_string (sample_format);
}

static ExportFormatBase::SampleFormat
parse_sample_format (std::string const& value)
{
	if (value == "16") {
		return ExportFormatBase::SF_16;
	}
	if (value == "24") {
		return ExportFormatBase::SF_24;
	}
	if (value == "32") {
		return ExportFormatBase::SF_32;
	}
	if (value == "float") {
		return ExportFormatBase::SF_Float;
	}
	throw std::runtime_error ("invalid render bitDepth: " + value);
}

static std::string
render_output_path (std::string const& outfile)
{
	std::string dirname  = Glib::path_get_dirname (outfile);
	std::string basename = Glib::path_get_basename (outfile);

	if (basename.size () > 4 && !basename.compare (basename.size () - 4, 4, ".wav")) {
		basename = PBD::basename_nosuffix (basename);
	}

	return Glib::build_filename (dirname, basename + ".wav");
}

static std::string
source_json (std::shared_ptr<Source> const& source)
{
	std::ostringstream out;
	std::shared_ptr<FileSource> file_source = std::dynamic_pointer_cast<FileSource> (source);

	out << "{\"id\":\"" << json_escape (source->id ().to_s ()) << "\""
	    << ",\"name\":\"" << json_escape (source->name ()) << "\""
	    << ",\"type\":\"" << data_type_name (source->type ()) << "\""
	    << ",\"length\":" << source->length ().samples ()
	    << ",\"naturalPosition\":" << source->natural_position ().samples ();

	if (file_source) {
		out << ",\"fileName\":\"" << json_escape (Glib::path_get_basename (file_source->path ())) << "\""
		    << ",\"withinSession\":" << (file_source->within_session () ? "true" : "false")
		    << ",\"channel\":" << file_source->channel ();
	}

	out << "}";
	return out.str ();
}

static std::string
region_json (std::shared_ptr<Region> const& region)
{
	std::ostringstream out;
	std::shared_ptr<AudioRegion> audio_region = std::dynamic_pointer_cast<AudioRegion> (region);
	uint32_t           source_count = audio_region ? audio_region->n_channels () : 1;

	out << "{\"id\":\"" << json_escape (region->id ().to_s ()) << "\""
	    << ",\"name\":\"" << json_escape (region->name ()) << "\""
	    << ",\"type\":\"" << data_type_name (region->data_type ()) << "\""
	    << ",\"position\":" << region->position_sample ()
	    << ",\"start\":" << region->start_sample ()
	    << ",\"length\":" << region->length_samples ()
	    << ",\"lastSample\":" << region->last_sample ()
	    << ",\"layer\":" << region->layer ()
	    << ",\"muted\":" << (region->muted () ? "true" : "false")
	    << ",\"opaque\":" << (region->opaque () ? "true" : "false")
	    << ",\"wholeFile\":" << (region->whole_file () ? "true" : "false")
	    << ",\"locked\":" << (region->locked () ? "true" : "false")
	    << ",\"positionLocked\":" << (region->position_locked () ? "true" : "false")
	    << ",\"sourceCount\":" << source_count
	    << ",\"sources\":[";

	for (uint32_t n = 0; n < source_count; ++n) {
		std::shared_ptr<Source> source = region->source (n);
		if (!source) {
			continue;
		}
		if (n > 0) {
			out << ",";
		}
		out << source_json (source);
	}

	out << "]}";
	return out.str ();
}

static std::string
playlist_json (std::shared_ptr<Playlist> const& playlist)
{
	std::ostringstream out;
	bool               first_region = true;

	out << "{\"id\":\"" << json_escape (playlist->id ().to_s ()) << "\""
	    << ",\"name\":\"" << json_escape (playlist->name ()) << "\""
	    << ",\"regions\":[";

	std::shared_ptr<RegionList> regions = playlist->region_list ();
	for (RegionList::const_iterator r = regions->begin (); r != regions->end (); ++r) {
		if (!*r) {
			continue;
		}
		if (!first_region) {
			out << ",";
		}
		first_region = false;
		out << region_json (*r);
	}

	out << "]}";
	return out.str ();
}

static bool
render_range_from_playlists (Session& session, samplepos_t& start, samplepos_t& end)
{
	std::shared_ptr<RouteList const> routes = session.get_routes ();
	bool                            have_range = false;

	if (!routes) {
		return false;
	}

	for (RouteList::const_iterator i = routes->begin (); i != routes->end (); ++i) {
		std::shared_ptr<AudioTrack> track = std::dynamic_pointer_cast<AudioTrack> (*i);
		if (!track || !track->playlist ()) {
			continue;
		}

		std::pair<timepos_t, timepos_t> extent = track->playlist ()->get_extent ();
		samplepos_t                     route_start = extent.first.samples ();
		samplepos_t                     route_end   = extent.second.samples ();
		if (route_end <= route_start) {
			continue;
		}

		if (!have_range) {
			start = route_start;
			end = route_end;
			have_range = true;
		} else {
			start = std::min (start, route_start);
			end = std::max (end, route_end);
		}
	}

	return have_range;
}

static uint32_t
register_master_export_channels (Session& session, std::shared_ptr<ExportChannelConfiguration> const& channels)
{
	if (!session.master_out ()) {
		return 0;
	}

	IO* master_out = session.master_out ()->output ().get ();
	if (!master_out) {
		return 0;
	}

	for (uint32_t n = 0; n < master_out->n_ports ().n_audio (); ++n) {
		PortExportChannel* channel = new PortExportChannel ();
		channel->add_port (master_out->audio (n));
		ExportChannelPtr channel_ptr (channel);
		channels->register_channel (channel_ptr);
	}

	return master_out->n_ports ().n_audio ();
}

static uint32_t
register_track_export_channels (Session& session, std::shared_ptr<ExportChannelConfiguration> const& channels)
{
	std::shared_ptr<RouteList const> routes = session.get_routes ();
	uint32_t                        max_audio_outputs = 0;

	if (!routes) {
		return 0;
	}

	for (RouteList::const_iterator i = routes->begin (); i != routes->end (); ++i) {
		std::shared_ptr<AudioTrack> track = std::dynamic_pointer_cast<AudioTrack> (*i);
		if (!track || !track->output ()) {
			continue;
		}
		max_audio_outputs = std::max (max_audio_outputs, track->output ()->n_ports ().n_audio ());
	}

	for (uint32_t n = 0; n < max_audio_outputs; ++n) {
		PortExportChannel* channel = new PortExportChannel ();
		bool               added = false;

		for (RouteList::const_iterator i = routes->begin (); i != routes->end (); ++i) {
			std::shared_ptr<AudioTrack> track = std::dynamic_pointer_cast<AudioTrack> (*i);
			if (!track || !track->output () || track->output ()->n_ports ().n_audio () <= n) {
				continue;
			}
			channel->add_port (track->output ()->audio (n));
			added = true;
		}

		if (added) {
			ExportChannelPtr channel_ptr (channel);
			channels->register_channel (channel_ptr);
		} else {
			delete channel;
		}
	}

	return max_audio_outputs;
}

static int
render_session (Session& session, std::string const& outfile, RenderSettings const& settings)
{
	ExportTimespanPtr                         timespan = session.get_export_handler ()->add_timespan ();
	std::shared_ptr<ExportChannelConfiguration> channels = session.get_export_handler ()->add_channel_config ();
	std::shared_ptr<ExportFilename>           filename = session.get_export_handler ()->add_filename ();
	std::shared_ptr<BroadcastInfo>            broadcast_info;
	int                                       sample_rate = settings.sample_rate ? settings.sample_rate : session.nominal_sample_rate ();

	XMLTree tree;
	std::ostringstream format_xml;
	format_xml
		<< "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
		<< "<ExportFormatSpecification name=\"RESON-WAV-EXPORT\" id=\"b1280899-0459-4aef-9dc9-7e2277fa6d24\">"
		<< "  <Encoding id=\"F_WAV\" type=\"T_Sndfile\" extension=\"wav\" name=\"WAV\" has-sample-format=\"true\" channel-limit=\"256\"/>"
		<< "  <SampleRate rate=\"" << sample_rate << "\"/>"
		<< "  <SRCQuality quality=\"SRC_SincBest\"/>"
		<< "  <EncodingOptions>"
		<< "    <Option name=\"sample-format\" value=\"" << sample_format_name (settings.sample_format) << "\"/>"
		<< "    <Option name=\"dithering\" value=\"D_None\"/>"
		<< "    <Option name=\"tag-metadata\" value=\"true\"/>"
		<< "    <Option name=\"tag-support\" value=\"false\"/>"
		<< "    <Option name=\"broadcast-info\" value=\"" << (settings.broadcast ? "true" : "false") << "\"/>"
		<< "  </EncodingOptions>"
		<< "  <Processing>"
		<< "    <Normalize enabled=\"" << (settings.normalize ? "true" : "false") << "\" target=\"0\"/>"
		<< "    <Silence>"
		<< "      <Start>"
		<< "        <Trim enabled=\"false\"/>"
		<< "        <Add enabled=\"false\">"
		<< "          <Duration format=\"Timecode\" hours=\"0\" minutes=\"0\" seconds=\"0\" frames=\"0\"/>"
		<< "        </Add>"
		<< "      </Start>"
		<< "      <End>"
		<< "        <Trim enabled=\"false\"/>"
		<< "        <Add enabled=\"false\">"
		<< "          <Duration format=\"Timecode\" hours=\"0\" minutes=\"0\" seconds=\"0\" frames=\"0\"/>"
		<< "        </Add>"
		<< "      </End>"
		<< "    </Silence>"
		<< "  </Processing>"
		<< "</ExportFormatSpecification>";

	tree.read_buffer (format_xml.str ().c_str ());
	std::shared_ptr<ExportFormatSpecification> format = session.get_export_handler ()->add_format (*tree.root ());

	samplepos_t start = session.current_start_sample ();
	samplepos_t end   = session.current_end_sample ();
	if (end <= start) {
		render_range_from_playlists (session, start, end);
	}
	if (end <= start) {
		throw std::runtime_error ("render range is empty");
	}

	timespan->set_range (start, end);
	timespan->set_range_id ("session");

	uint32_t channel_count = register_master_export_channels (session, channels);
	if (channel_count == 0) {
		channel_count = register_track_export_channels (session, channels);
	}
	if (channel_count == 0) {
		throw std::runtime_error ("render requires audio output ports");
	}

	std::string dirname  = Glib::path_get_dirname (outfile);
	std::string basename = Glib::path_get_basename (outfile);
	if (basename.size () > 4 && !basename.compare (basename.size () - 4, 4, ".wav")) {
		basename = PBD::basename_nosuffix (basename);
	}

	filename->set_folder (dirname);
	timespan->set_name (basename);

	if (settings.broadcast) {
		broadcast_info.reset (new BroadcastInfo);
		broadcast_info->set_from_session (session, timespan->get_start ());
	}

	filename->set_timespan (timespan);
	filename->include_label = false;
	format->set_soundcloud_upload (false);
	session.get_export_handler ()->add_export_config (timespan, channels, format, filename, broadcast_info);

	if (session.get_export_handler ()->do_export () != 0) {
		return -1;
	}

	std::shared_ptr<ExportStatus> status = session.get_export_status ();
	while (status->running ()) {
		Glib::usleep (100000);
	}
	status->finish (TRS_UI);

	return 0;
}

static std::string
observe_session_json (Session& session)
{
	std::shared_ptr<RouteList const> routes = session.get_routes ();
	std::ostringstream              out;
	bool                            first = true;

	out << "{\"sessionName\":\"" << json_escape (session.name ()) << "\"";
	out << ",\"sampleRate\":" << session.nominal_sample_rate ();
	out << ",\"routes\":[";

	if (routes) {
		for (RouteList::const_iterator i = routes->begin (); i != routes->end (); ++i) {
			std::shared_ptr<Route> route = *i;
			if (!route) {
				continue;
			}
			if (!first) {
				out << ",";
			}
			first = false;
			out << "{\"id\":\"" << json_escape (route->id ().to_s ()) << "\""
			    << ",\"name\":\"" << json_escape (route->name ()) << "\""
			    << ",\"type\":\"" << route_type (route) << "\""
			    << ",\"hidden\":" << (route->is_hidden () ? "true" : "false");
			std::shared_ptr<AudioTrack> audio_track = std::dynamic_pointer_cast<AudioTrack> (route);
			if (audio_track && audio_track->playlist ()) {
				out << ",\"playlist\":" << playlist_json (audio_track->playlist ());
			}
			out << "}";
		}
	}

	out << "]}";
	return out.str ();
}

static std::string
observe_hash (Session* session)
{
	if (!session) {
		return "";
	}
	return sha256_json (observe_session_json (*session));
}

static std::string
journal_snapshot_path (std::string const& journal_path)
{
	std::string dirname = Glib::path_get_dirname (journal_path);
	std::string journal_name = PBD::basename_nosuffix (Glib::path_get_basename (journal_path));
	std::string snapshot_name = journal_name + "_batch_0001_before.tar.gz";
	if (dirname == ".") {
		return Glib::build_filename (".reson", "snapshots", snapshot_name);
	}
	return Glib::build_filename (dirname, "snapshots", snapshot_name);
}

static void
archive_check (int rc, archive* ar, std::string const& action)
{
	if (rc != ARCHIVE_OK) {
		std::string detail = archive_error_string (ar) ? archive_error_string (ar) : "unknown archive error";
		throw std::runtime_error (action + ": " + detail);
	}
}

static void
write_file_to_archive (archive* ar, fs::path const& file_path)
{
	std::ifstream file (file_path, std::ios::in | std::ios::binary);
	if (!file) {
		throw std::runtime_error ("cannot read snapshot file: " + file_path.string ());
	}

	char buffer[16384];
	while (file) {
		file.read (buffer, sizeof (buffer));
		std::streamsize count = file.gcount ();
		if (count > 0 && archive_write_data (ar, buffer, count) < 0) {
			throw std::runtime_error ("write snapshot data: " + std::string (archive_error_string (ar)));
		}
	}
}

static void
capture_session_snapshot (std::string const& session_dir, SnapshotInfo& snapshot)
{
	if (snapshot.captured) {
		return;
	}
	if (session_dir.empty ()) {
		throw std::runtime_error ("cannot capture snapshot without a sessionDir");
	}
	if (!fs::exists (session_dir)) {
		throw std::runtime_error ("cannot capture snapshot; sessionDir does not exist: " + session_dir);
	}

	std::string snapshot_dir = Glib::path_get_dirname (snapshot.path);
	if (!snapshot_dir.empty () && snapshot_dir != ".") {
		g_mkdir_with_parents (snapshot_dir.c_str (), 0755);
	}

	archive* ar = archive_write_new ();
	if (!ar) {
		throw std::runtime_error ("cannot allocate snapshot archive");
	}

	try {
		archive_check (archive_write_add_filter_gzip (ar), ar, "enable gzip snapshot filter");
		archive_check (archive_write_set_format_pax_restricted (ar), ar, "set snapshot archive format");
		archive_check (archive_write_open_filename (ar, snapshot.path.c_str ()), ar, "open snapshot archive");

		fs::path root (session_dir);
		for (fs::recursive_directory_iterator i (root), end; i != end; ++i) {
			fs::path path = i->path ();
			fs::path rel  = fs::relative (path, root);
			if (rel.empty ()) {
				continue;
			}

			archive_entry* entry = archive_entry_new ();
			if (!entry) {
				throw std::runtime_error ("cannot allocate snapshot archive entry");
			}

			std::string rel_name = rel.generic_string ();
			archive_entry_set_pathname (entry, rel_name.c_str ());
			if (fs::is_directory (path)) {
				archive_entry_set_filetype (entry, AE_IFDIR);
				archive_entry_set_perm (entry, 0755);
				archive_entry_set_size (entry, 0);
				archive_check (archive_write_header (ar, entry), ar, "write snapshot directory header");
			} else if (fs::is_regular_file (path)) {
				archive_entry_set_filetype (entry, AE_IFREG);
				archive_entry_set_perm (entry, 0644);
				archive_entry_set_size (entry, fs::file_size (path));
				archive_check (archive_write_header (ar, entry), ar, "write snapshot file header");
				write_file_to_archive (ar, path);
			}
			archive_entry_free (entry);
		}

		archive_check (archive_write_close (ar), ar, "close snapshot archive");
		archive_write_free (ar);
		ar = 0;
		snapshot.sha256 = sha256_file (snapshot.path);
		snapshot.captured = true;
	} catch (...) {
		if (ar) {
			archive_write_close (ar);
			archive_write_free (ar);
		}
		throw;
	}
}

static bool
has_suffix (std::string const& value, std::string const& suffix)
{
	return value.size () >= suffix.size () && value.compare (value.size () - suffix.size (), suffix.size (), suffix) == 0;
}

static void
prune_snapshots (std::string const& snapshot_path, uint32_t max_count)
{
	if (max_count == 0) {
		return;
	}

	fs::path snapshot_dir = fs::path (snapshot_path).parent_path ();
	if (snapshot_dir.empty () || !fs::exists (snapshot_dir)) {
		return;
	}

	std::vector<std::pair<fs::file_time_type, fs::path> > snapshots;
	for (fs::directory_iterator i (snapshot_dir), end; i != end; ++i) {
		if (!fs::is_regular_file (i->path ())) {
			continue;
		}
		std::string name = i->path ().filename ().string ();
		if (!has_suffix (name, "_batch_0001_before.tar.gz")) {
			continue;
		}
		snapshots.push_back (std::make_pair (fs::last_write_time (i->path ()), i->path ()));
	}

	std::sort (snapshots.begin (), snapshots.end ());
	while (snapshots.size () > max_count) {
		fs::remove (snapshots.front ().second);
		snapshots.erase (snapshots.begin ());
	}
}

static bool
safe_archive_path (std::string const& name)
{
	fs::path path (name);
	if (path.is_absolute ()) {
		return false;
	}
	for (fs::path::const_iterator i = path.begin (); i != path.end (); ++i) {
		if (*i == "..") {
			return false;
		}
	}
	return true;
}

static void
restore_session_snapshot (std::string const& snapshot_path, std::string const& session_dir)
{
	if (snapshot_path.empty () || session_dir.empty ()) {
		throw std::runtime_error ("restore_batch_snapshot requires snapshotPath and sessionDir");
	}
	if (!fs::exists (snapshot_path)) {
		throw std::runtime_error ("snapshotPath does not exist: " + snapshot_path);
	}

	fs::remove_all (session_dir);
	fs::create_directories (session_dir);

	archive* ar = archive_read_new ();
	if (!ar) {
		throw std::runtime_error ("cannot allocate snapshot reader");
	}

	try {
		archive_read_support_format_tar (ar);
		archive_read_support_filter_gzip (ar);
		archive_check (archive_read_open_filename (ar, snapshot_path.c_str (), 16384), ar, "open snapshot archive");

		archive_entry* entry = 0;
		while (true) {
			int rc = archive_read_next_header (ar, &entry);
			if (rc == ARCHIVE_EOF) {
				break;
			}
			archive_check (rc, ar, "read snapshot entry");

			std::string rel_name = archive_entry_pathname (entry) ? archive_entry_pathname (entry) : "";
			if (!safe_archive_path (rel_name)) {
				throw std::runtime_error ("unsafe snapshot entry path: " + rel_name);
			}

			fs::path target = fs::path (session_dir) / rel_name;
			if (archive_entry_filetype (entry) == AE_IFDIR) {
				fs::create_directories (target);
			} else if (archive_entry_filetype (entry) == AE_IFREG) {
				fs::create_directories (target.parent_path ());
				std::ofstream file (target, std::ios::out | std::ios::binary | std::ios::trunc);
				if (!file) {
					throw std::runtime_error ("cannot restore snapshot file: " + target.string ());
				}
				char buffer[16384];
				while (true) {
					ssize_t count = archive_read_data (ar, buffer, sizeof (buffer));
					if (count == 0) {
						break;
					}
					if (count < 0) {
						throw std::runtime_error ("read snapshot data: " + std::string (archive_error_string (ar)));
					}
					file.write (buffer, count);
				}
			}
		}

		archive_check (archive_read_close (ar), ar, "close snapshot archive");
		archive_read_free (ar);
	} catch (...) {
		archive_read_close (ar);
		archive_read_free (ar);
		throw;
	}
}

static void
append_touched (std::ostringstream& out, std::string const& key, std::string const& value, bool& first)
{
	if (value.empty ()) {
		return;
	}
	if (!first) {
		out << ",";
	}
	first = false;
	out << "\"" << key << "\":[\"" << json_escape (value) << "\"]";
}

static std::string
touched_json (JournalEntry const& entry)
{
	std::ostringstream out;
	bool               first = true;

	out << "{";
	append_touched (out, "tracks", entry.touched_tracks, first);
	append_touched (out, "playlists", entry.touched_playlists, first);
	append_touched (out, "regions", entry.touched_regions, first);
	append_touched (out, "sources", entry.touched_sources, first);
	out << "}";
	return out.str ();
}

static void
write_command_journal (
	std::string const&              journal_path,
	std::string const&              created_at,
	std::string const&              batch_status,
	Session*                        session,
	SnapshotInfo const&             snapshot,
	std::vector<JournalEntry> const& entries)
{
	std::string dirname = Glib::path_get_dirname (journal_path);
	if (!dirname.empty () && dirname != ".") {
		g_mkdir_with_parents (dirname.c_str (), 0755);
	}

	std::ostringstream out;
	out << "{\"schemaVersion\":\"reson.command_journal.v0\""
	    << ",\"journalId\":\"" << json_escape (PBD::basename_nosuffix (Glib::path_get_basename (journal_path))) << "\""
	    << ",\"session\":{";
	if (session) {
		out << "\"name\":\"" << json_escape (session->name ()) << "\""
		    << ",\"sampleRate\":" << session->nominal_sample_rate ();
	}
	out << "}"
	    << ",\"createdAt\":\"" << json_escape (created_at) << "\""
	    << ",\"bridge\":{\"name\":\"ardour-session-utils\",\"version\":\"spike\",\"engineVersion\":\"" << json_escape (VERSIONSTRING) << "\"}"
	    << ",\"batches\":[{\"batchId\":\"batch_0001\""
	    << ",\"reason\":\"command-file\""
	    << ",\"risk\":\"normal\""
	    << ",\"startedAt\":\"" << json_escape (created_at) << "\""
	    << ",\"completedAt\":\"" << json_escape (utc_now_json ()) << "\""
	    << ",\"status\":\"" << json_escape (batch_status) << "\""
	    << ",\"preState\":{\"snapshot\":{\"kind\":\"session_archive\",\"path\":\"" << json_escape (snapshot.path) << "\"";
	if (!snapshot.sha256.empty ()) {
		out << ",\"sha256\":\"" << json_escape (snapshot.sha256) << "\"";
	}
	out << "}}"
	    << ",\"postState\":{";
	if (session) {
		out << "\"observationHash\":\"" << observe_hash (session) << "\"";
	}
	out << "},\"entries\":[";

	for (std::vector<JournalEntry>::const_iterator i = entries.begin (); i != entries.end (); ++i) {
		if (i != entries.begin ()) {
			out << ",";
		}
		out << "{\"entryId\":\"" << json_escape (i->entry_id) << "\""
		    << ",\"op\":\"" << json_escape (i->op) << "\""
		    << ",\"commandSchemaVersion\":\"reson.command.v0\""
		    << ",\"command\":" << i->command_json
		    << ",\"status\":\"" << json_escape (i->status) << "\""
		    << ",\"startedAt\":\"" << json_escape (created_at) << "\""
		    << ",\"completedAt\":\"" << json_escape (created_at) << "\""
		    << ",\"touched\":" << touched_json (*i)
		    << ",\"preState\":{";
		if (!i->pre_hash.empty ()) {
			out << "\"observationHash\":\"" << i->pre_hash << "\"";
		}
		out << "},\"postState\":{";
		if (!i->post_hash.empty ()) {
			out << "\"observationHash\":\"" << i->post_hash << "\"";
		}
		out << "},\"rollback\":{\"kind\":\"restore_batch_snapshot\",\"batchId\":\"batch_0001\"}";
		if (!i->error_message.empty ()) {
			out << ",\"error\":{\"message\":\"" << json_escape (i->error_message) << "\"}";
		}
		out << "}";
	}

	out << "]}]}";

	std::ofstream journal (journal_path.c_str (), std::ios::out | std::ios::trunc);
	if (!journal) {
		throw std::runtime_error ("cannot write journal: " + journal_path);
	}
	journal << out.str () << "\n";
}

static void
require_session (Session* session, std::string const& op)
{
	if (!session) {
		throw std::runtime_error ("operation '" + op + "' requires an active session");
	}
}

static std::string
default_session_name (std::string const& session_dir)
{
	return Glib::path_get_basename (session_dir);
}

static void
usage ()
{
	std::cout << UTILNAME << " - run Reson JSON commands against an Ardour session.\n\n";
	std::cout << "Usage: " << UTILNAME << " <command-file.json>\n\n";
	std::cout << "Supported operations:\n";
	std::cout << "  create_session, open_session, restore_batch_snapshot, create_audio_track, import_audio, place_audio, render, save_session, observe_session\n\n";
	std::cout << "Example:\n";
	std::cout << "  " << UTILNAME << " /tmp/reson-command.json\n";
	::exit (EXIT_SUCCESS);
}

int
main (int argc, char* argv[])
{
	if (argc != 2 || std::string (argv[1]) == "-h" || std::string (argv[1]) == "--help") {
		usage ();
	}

	pt::ptree root;
	try {
		pt::read_json (argv[1], root);
	} catch (std::exception const& e) {
		std::cerr << "Error: cannot read command file: " << e.what () << "\n";
		return EXIT_FAILURE;
	}

	SessionUtils::init (false);
	Session*           session = 0;
	std::ostringstream result;
	bool               first_result = true;
	std::string        journal_path = root.get<std::string> ("journalPath", "");
	uint32_t           snapshot_retention_max_count = root.get<uint32_t> ("snapshotRetention.maxCount", 0);
	std::string        journal_started_at = utc_now_json ();
	std::vector<JournalEntry> journal_entries;
	uint32_t           journal_entry_count = 0;
	std::string        current_session_dir;
	SnapshotInfo       snapshot;
	JournalEntry       journal_entry;
	bool               journal_entry_pending = false;

	if (!journal_path.empty ()) {
		snapshot.path = journal_snapshot_path (journal_path);
	}

	result << "{\"schemaVersion\":\"reson.result.v0\",\"results\":[";

	try {
		pt::ptree const& commands = root.get_child ("commands");

		for (pt::ptree::const_iterator i = commands.begin (); i != commands.end (); ++i) {
			pt::ptree const& command = i->second;
			std::string      op      = command.get<std::string> ("op");
			journal_entry = JournalEntry ();
			journal_entry_pending = false;

			if (!journal_path.empty ()) {
				++journal_entry_count;
				std::ostringstream entry_id;
				entry_id << "entry_" << std::setfill ('0') << std::setw (4) << journal_entry_count;
				journal_entry.entry_id = entry_id.str ();
				journal_entry.op = op;
				journal_entry.command_json = ptree_json (command);
				journal_entry.status = "applied";
				journal_entry.pre_hash = observe_hash (session);
				journal_entry_pending = true;
			}

			if (!first_result) {
				result << ",";
			}
			first_result = false;
			result << "{\"op\":\"" << json_escape (op) << "\"";

			if (op == "create_session") {
				std::string session_dir  = command.get<std::string> ("sessionDir");
				std::string session_name = command.get<std::string> ("sessionName", default_session_name (session_dir));
				float       sample_rate  = command.get<float> ("sampleRate", 48000.0f);

				if (session) {
					SessionUtils::unload_session (session);
					session = 0;
				}

				session = SessionUtils::create_session (session_dir, session_name, sample_rate);
				if (!session) {
					throw std::runtime_error ("create_session failed");
				}
				current_session_dir = session_dir;
				if (!journal_path.empty ()) {
					if (session->save_state ("") != 0) {
						throw std::runtime_error ("create_session snapshot save failed");
					}
					capture_session_snapshot (current_session_dir, snapshot);
					prune_snapshots (snapshot.path, snapshot_retention_max_count);
				}

				result << ",\"ok\":true,\"sessionDir\":\"" << json_escape (session_dir) << "\""
				       << ",\"sessionName\":\"" << json_escape (session_name) << "\"";

			} else if (op == "open_session") {
				std::string session_dir  = command.get<std::string> ("sessionDir");
				std::string session_name = command.get<std::string> ("sessionName", default_session_name (session_dir));

				if (session) {
					SessionUtils::unload_session (session);
					session = 0;
				}

				session = SessionUtils::load_session (session_dir, session_name);
				if (!session) {
					throw std::runtime_error ("open_session failed");
				}
				current_session_dir = session_dir;
				if (!journal_path.empty ()) {
					capture_session_snapshot (current_session_dir, snapshot);
					prune_snapshots (snapshot.path, snapshot_retention_max_count);
				}

				result << ",\"ok\":true,\"sessionDir\":\"" << json_escape (session_dir) << "\""
				       << ",\"sessionName\":\"" << json_escape (session_name) << "\"";

			} else if (op == "restore_batch_snapshot") {
				std::string snapshot_path = command.get<std::string> ("snapshotPath");
				std::string session_dir   = command.get<std::string> ("sessionDir");
				std::string session_name  = command.get<std::string> ("sessionName", default_session_name (session_dir));

				if (session) {
					SessionUtils::unload_session (session);
					session = 0;
				}

				restore_session_snapshot (snapshot_path, session_dir);
				current_session_dir = session_dir;
				result << ",\"ok\":true,\"sessionDir\":\"" << json_escape (session_dir) << "\""
				       << ",\"sessionName\":\"" << json_escape (session_name) << "\""
				       << ",\"snapshotPath\":\"" << json_escape (snapshot_path) << "\"";

			} else if (op == "create_audio_track") {
				require_session (session, op);

				int         input_channels  = command.get<int> ("inputChannels", 1);
				int         output_channels = command.get<int> ("outputChannels", 2);
				uint32_t    count           = command.get<uint32_t> ("count", 1);
				std::string name            = command.get<std::string> ("name", "Audio");

				AudioTrackList tracks = session->new_audio_track (
					input_channels,
					output_channels,
					std::shared_ptr<RouteGroup> (),
					count,
					name,
					PresentationInfo::max_order,
					Normal);

				result << ",\"ok\":true,\"count\":" << tracks.size () << ",\"tracks\":[";
				bool first_track = true;
				for (AudioTrackList::const_iterator t = tracks.begin (); t != tracks.end (); ++t) {
					if (!first_track) {
						result << ",";
					}
					first_track = false;
					if (!journal_path.empty () && journal_entry.touched_tracks.empty ()) {
						journal_entry.touched_tracks = (*t)->id ().to_s ();
						if ((*t)->playlist ()) {
							journal_entry.touched_playlists = (*t)->playlist ()->id ().to_s ();
						}
					}
					result << "{\"id\":\"" << json_escape ((*t)->id ().to_s ()) << "\""
					       << ",\"name\":\"" << json_escape ((*t)->name ()) << "\"}";
				}
				result << "]";

			} else if (op == "import_audio") {
				require_session (session, op);

				std::string path         = command.get<std::string> ("path");
				std::string track_id     = command.get<std::string> ("trackId", "");
				std::string track_name   = command.get<std::string> ("trackName", "");
				std::string region_name  = command.get<std::string> ("regionName", "");
				std::string start_value  = command.get<std::string> ("start", "0");
				bool        create_track = command.get<bool> ("createTrack", false);
				samplepos_t start        = parse_position (*session, start_value);

				ImportStatus status;
				status.current                 = 0;
				status.total                   = 1;
				status.quality                 = SrcBest;
				status.freeze                  = false;
				status.replace_existing_source = false;
				status.split_midi_channels     = false;
				status.import_markers          = false;
				status.midi_track_name_source  = SMFFileAndTrackName;
				status.all_done                = false;
				status.paths.push_back (path);

				session->import_files (status);

				SourceList sources;
				for (SourceList::const_iterator s = status.sources.begin (); s != status.sources.end (); ++s) {
					if (*s && !(*s)->empty ()) {
						sources.push_back (*s);
					}
				}
				if (sources.empty ()) {
					throw std::runtime_error ("import_audio produced no usable sources: " + path);
				}

				if (region_name.empty ()) {
					region_name = region_name_from_path (path, sources.size () > 1, false);
				}
				region_name = unique_region_name (region_name);

				std::shared_ptr<AudioTrack> track = find_audio_track (*session, track_id, track_name);
				if (!track && create_track) {
					std::string new_track_name = track_name.empty () ? region_name : track_name;
					AudioTrackList tracks      = session->new_audio_track (
						sources.size (),
						2,
						std::shared_ptr<RouteGroup> (),
						1,
						new_track_name,
						PresentationInfo::max_order,
						Normal);
					if (!tracks.empty ()) {
						track = tracks.front ();
					}
				}
				if (!track) {
					throw std::runtime_error ("import_audio requires an existing audio track or createTrack=true");
				}

				PBD::PropertyList plist;
				plist.add (Properties::start, timecnt_t (Temporal::AudioTime));
				plist.add (Properties::length, sources[0]->length ());
				plist.add (Properties::name, region_name);
				plist.add (Properties::layer, 0);
				plist.add (Properties::whole_file, true);
				plist.add (Properties::external, true);
				plist.add (Properties::opaque, true);

				std::shared_ptr<Region> region = RegionFactory::create (sources, plist);
				if (!region) {
					throw std::runtime_error ("import_audio failed to create region");
				}
				std::shared_ptr<AudioRegion> audio_region = std::dynamic_pointer_cast<AudioRegion> (region);
				if (audio_region) {
					audio_region->special_set_position (sources[0]->natural_position ());
				}

				std::shared_ptr<Playlist> playlist = track->playlist ();
				if (!playlist) {
					throw std::runtime_error ("target audio track has no playlist");
				}
				std::shared_ptr<Region> copy = RegionFactory::create (region, region->derive_properties ());
				playlist->clear_changes ();
				playlist->clear_owned_changes ();
				playlist->add_region (copy, timepos_t (start));
				if (!journal_path.empty ()) {
					journal_entry.touched_tracks = track->id ().to_s ();
					journal_entry.touched_playlists = playlist->id ().to_s ();
					journal_entry.touched_regions = copy->id ().to_s ();
					journal_entry.touched_sources = sources[0]->id ().to_s ();
				}

				result << ",\"ok\":true,\"path\":\"" << json_escape (path) << "\""
				       << ",\"sourceCount\":" << sources.size ()
				       << ",\"trackId\":\"" << json_escape (track->id ().to_s ()) << "\""
				       << ",\"trackName\":\"" << json_escape (track->name ()) << "\""
				       << ",\"regionId\":\"" << json_escape (copy->id ().to_s ()) << "\""
				       << ",\"regionName\":\"" << json_escape (copy->name ()) << "\""
				       << ",\"start\":" << start;

			} else if (op == "place_audio") {
				require_session (session, op);

				std::string region_id   = command.get<std::string> ("regionId", "");
				std::string region_name = command.get<std::string> ("regionName", "");
				std::string track_id    = command.get<std::string> ("trackId", "");
				std::string track_name  = command.get<std::string> ("trackName", "");
				std::string start_value = command.get<std::string> ("start");
				samplepos_t start       = parse_position (*session, start_value);

				if (region_id.empty () && region_name.empty ()) {
					throw std::runtime_error ("place_audio requires regionId or regionName");
				}

				RegionLocation location = find_region_location (*session, region_id, region_name);
				if (!location.region || !location.playlist || !location.track) {
					throw std::runtime_error ("place_audio region not found");
				}

				std::shared_ptr<AudioTrack> target_track = location.track;
				if (!track_id.empty () || !track_name.empty ()) {
					target_track = find_audio_track (*session, track_id, track_name);
					if (!target_track) {
						throw std::runtime_error ("place_audio target track not found");
					}
				}
				if (!target_track->playlist ()) {
					throw std::runtime_error ("place_audio target track has no playlist");
				}

				if (target_track->playlist () == location.playlist) {
					location.playlist->clear_changes ();
					location.playlist->clear_owned_changes ();
					location.region->set_position (timepos_t (start));
				} else {
					location.playlist->clear_changes ();
					location.playlist->clear_owned_changes ();
					target_track->playlist ()->clear_changes ();
					target_track->playlist ()->clear_owned_changes ();
					location.playlist->remove_region (location.region);
					target_track->playlist ()->add_region (location.region, timepos_t (start));
				}
				if (!journal_path.empty ()) {
					journal_entry.touched_tracks = target_track->id ().to_s ();
					journal_entry.touched_playlists = target_track->playlist ()->id ().to_s ();
					journal_entry.touched_regions = location.region->id ().to_s ();
				}

				result << ",\"ok\":true,\"regionId\":\"" << json_escape (location.region->id ().to_s ()) << "\""
				       << ",\"regionName\":\"" << json_escape (location.region->name ()) << "\""
				       << ",\"trackId\":\"" << json_escape (target_track->id ().to_s ()) << "\""
				       << ",\"trackName\":\"" << json_escape (target_track->name ()) << "\""
				       << ",\"start\":" << start;

			} else if (op == "save_session") {
				require_session (session, op);

				if (session->save_state ("") != 0) {
					throw std::runtime_error ("save_session failed");
				}
				result << ",\"ok\":true";

			} else if (op == "render") {
				require_session (session, op);

				std::string    output_path = command.get<std::string> ("outputPath");
				RenderSettings settings;
				settings.sample_rate   = command.get<int> ("sampleRate", session->nominal_sample_rate ());
				settings.sample_format = parse_sample_format (command.get<std::string> ("bitDepth", "16"));
				settings.normalize     = command.get<bool> ("normalize", false);
				settings.broadcast     = command.get<bool> ("broadcast", false);

				if (settings.sample_rate < 8000 || settings.sample_rate > 192000) {
					throw std::runtime_error ("invalid render sampleRate");
				}

				if (render_session (*session, output_path, settings) != 0) {
					throw std::runtime_error ("render failed");
				}

				result << ",\"ok\":true,\"outputPath\":\"" << json_escape (render_output_path (output_path)) << "\""
				       << ",\"sampleRate\":" << settings.sample_rate
				       << ",\"bitDepth\":\"" << json_escape (command.get<std::string> ("bitDepth", "16")) << "\"";

			} else if (op == "observe_session") {
				require_session (session, op);

				result << ",\"ok\":true,\"observation\":" << observe_session_json (*session);

			} else {
				throw std::runtime_error ("unsupported operation: " + op);
			}

			if (!journal_path.empty ()) {
				journal_entry.post_hash = observe_hash (session);
				journal_entries.push_back (journal_entry);
				journal_entry_pending = false;
			}

			result << "}";
		}

		if (!journal_path.empty ()) {
			write_command_journal (journal_path, journal_started_at, "applied", session, snapshot, journal_entries);
		}

	} catch (std::exception const& e) {
		if (!journal_path.empty ()) {
			if (journal_entry_pending) {
				journal_entry.status = "failed";
				journal_entry.error_message = e.what ();
				journal_entry.post_hash = observe_hash (session);
				journal_entries.push_back (journal_entry);
				journal_entry_pending = false;
			}
			try {
				write_command_journal (journal_path, journal_started_at, "failed", session, snapshot, journal_entries);
			} catch (std::exception const& journal_error) {
				std::cerr << "Error: cannot write failed journal: " << journal_error.what () << "\n";
			}
		}
		if (session) {
			SessionUtils::unload_session (session);
		}
		SessionUtils::cleanup ();
		std::cerr << "Error: " << e.what () << "\n";
		return EXIT_FAILURE;
	}

	if (session) {
		SessionUtils::unload_session (session);
	}
	SessionUtils::cleanup ();

	result << "]}";
	std::cout << result.str () << "\n";
	return EXIT_SUCCESS;
}
