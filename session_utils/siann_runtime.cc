/*
 * Copyright (C) 2026 Gary Hsieh <romanticamaj@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
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

#include "ardour/audio_track.h"
#include "ardour/audioregion.h"
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
#include "ardour/route.h"
#include "ardour/region_factory.h"
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

struct SnapshotInfo
{
	std::string path;
	std::string sha256;
	bool        captured = false;
};

struct RollbackPoint
{
	std::string rollback_id;
	std::string snapshot_path;
	std::string snapshot_sha256;
	std::string session_dir;
	std::string session_name;
	std::string pre_hash;
	std::string post_hash;
};

static double
parse_time_number (std::string const& value)
{
	char* end = 0;
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

static samplepos_t
parse_position (Session& session, std::string const& value)
{
	std::vector<double> parts;
	std::string current;

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
runtime_region_name_from_path (std::string const& path, bool multiple_sources, bool copy)
{
	std::string name = PBD::basename_nosuffix (Glib::path_get_basename (path));
	if (multiple_sources) {
		name += " (stereo)";
	}
	if (copy) {
		name += " copy";
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
	std::string dirname = Glib::path_get_dirname (outfile);
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

	out << "{";
	if (file_source) {
		out << "\"channel\":" << file_source->channel () << ","
		    << "\"fileName\":\"" << json_escape (Glib::path_get_basename (file_source->path ())) << "\",";
	}
	out << "\"id\":\"" << json_escape (source->id ().to_s ()) << "\""
	    << ",\"length\":" << source->length ().samples ()
	    << ",\"name\":\"" << json_escape (source->name ()) << "\""
	    << ",\"naturalPosition\":" << source->natural_position ().samples ()
	    << ",\"type\":\"" << data_type_name (source->type ()) << "\"";
	if (file_source) {
		out << ",\"withinSession\":" << (file_source->within_session () ? "true" : "false");
	}
	out << "}";
	return out.str ();
}

static std::string
region_json (std::shared_ptr<Region> const& region)
{
	std::ostringstream out;
	std::shared_ptr<AudioRegion> audio_region = std::dynamic_pointer_cast<AudioRegion> (region);
	uint32_t source_count = audio_region ? audio_region->n_channels () : 1;

	out << "{\"id\":\"" << json_escape (region->id ().to_s ()) << "\""
	    << ",\"lastSample\":" << region->last_sample ()
	    << ",\"layer\":" << region->layer ()
	    << ",\"length\":" << region->length_samples ()
	    << ",\"locked\":" << (region->locked () ? "true" : "false")
	    << ",\"muted\":" << (region->muted () ? "true" : "false")
	    << ",\"name\":\"" << json_escape (region->name ()) << "\""
	    << ",\"opaque\":" << (region->opaque () ? "true" : "false")
	    << ",\"position\":" << region->position_sample ()
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

	out << "]"
	    << ",\"start\":" << region->start_sample ()
	    << ",\"type\":\"" << data_type_name (region->data_type ()) << "\""
	    << ",\"wholeFile\":" << (region->whole_file () ? "true" : "false")
	    << "}";
	return out.str ();
}

static std::string
playlist_json (std::shared_ptr<Playlist> const& playlist)
{
	std::ostringstream out;
	bool first_region = true;

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

static std::string
observe_session_json (Session& session)
{
	std::shared_ptr<RouteList const> routes = session.get_routes ();
	std::ostringstream out;
	bool first = true;

	out << "{\"routes\":[";
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
			out << "{\"hidden\":" << (route->is_hidden () ? "true" : "false")
			    << ",\"id\":\"" << json_escape (route->id ().to_s ()) << "\""
			    << ",\"name\":\"" << json_escape (route->name ()) << "\"";
			std::shared_ptr<AudioTrack> audio_track = std::dynamic_pointer_cast<AudioTrack> (route);
			if (audio_track && audio_track->playlist ()) {
				out << ",\"playlist\":" << playlist_json (audio_track->playlist ());
			}
			out << ",\"type\":\"" << route_type (route) << "\"";
			out << "}";
		}
	}

	out << "]"
	    << ",\"sampleRate\":" << session.nominal_sample_rate ()
	    << ",\"sessionName\":\"" << json_escape (session.name ()) << "\""
	    << "}";
	return out.str ();
}

static bool
render_range_from_playlists (Session& session, samplepos_t& start, samplepos_t& end)
{
	std::shared_ptr<RouteList const> routes = session.get_routes ();
	bool have_range = false;

	if (!routes) {
		return false;
	}

	for (RouteList::const_iterator i = routes->begin (); i != routes->end (); ++i) {
		std::shared_ptr<AudioTrack> track = std::dynamic_pointer_cast<AudioTrack> (*i);
		if (!track || !track->playlist ()) {
			continue;
		}
		std::pair<timepos_t, timepos_t> extent = track->playlist ()->get_extent ();
		samplepos_t route_start = extent.first.samples ();
		samplepos_t route_end = extent.second.samples ();
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
	uint32_t max_audio_outputs = 0;

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
		bool added = false;

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
	ExportTimespanPtr timespan = session.get_export_handler ()->add_timespan ();
	std::shared_ptr<ExportChannelConfiguration> channels = session.get_export_handler ()->add_channel_config ();
	std::shared_ptr<ExportFilename> filename = session.get_export_handler ()->add_filename ();
	std::shared_ptr<BroadcastInfo> broadcast_info;
	int sample_rate = settings.sample_rate ? settings.sample_rate : session.nominal_sample_rate ();

	XMLTree tree;
	std::ostringstream format_xml;
	format_xml
		<< "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
		<< "<ExportFormatSpecification name=\"SIANN-WAV-EXPORT\" id=\"b1280899-0459-4aef-9dc9-7e2277fa6d24\">"
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
		<< "      <Start><Trim enabled=\"false\"/><Add enabled=\"false\"><Duration format=\"Timecode\" hours=\"0\" minutes=\"0\" seconds=\"0\" frames=\"0\"/></Add></Start>"
		<< "      <End><Trim enabled=\"false\"/><Add enabled=\"false\"><Duration format=\"Timecode\" hours=\"0\" minutes=\"0\" seconds=\"0\" frames=\"0\"/></Add></End>"
		<< "    </Silence>"
		<< "  </Processing>"
		<< "</ExportFormatSpecification>";

	tree.read_buffer (format_xml.str ().c_str ());
	std::shared_ptr<ExportFormatSpecification> format = session.get_export_handler ()->add_format (*tree.root ());

	samplepos_t start = session.current_start_sample ();
	samplepos_t end = session.current_end_sample ();
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

	std::string dirname = Glib::path_get_dirname (outfile);
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
observe_hash (Session* session)
{
	if (!session) {
		return "";
	}
	return sha256_json (observe_session_json (*session));
}

static std::string
default_session_name (std::string const& session_dir)
{
	return Glib::path_get_basename (session_dir);
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
			fs::path rel = fs::relative (path, root);
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
		throw std::runtime_error ("restore snapshot requires snapshotPath and sessionDir");
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
require_session_id (std::string const& request_session_id, std::string const& active_session_id)
{
	if (active_session_id.empty () || request_session_id != active_session_id) {
		throw std::runtime_error ("session_mismatch: unknown sessionId");
	}
}

static void
write_response (std::string const& request_id, bool ok, std::string const& type, std::string const& body_json)
{
	std::cout << "{\"requestId\":\"" << json_escape (request_id) << "\""
	          << ",\"ok\":" << (ok ? "true" : "false")
	          << ",\"type\":\"" << json_escape (type) << "\""
	          << ",\"body\":" << body_json
	          << "}" << std::endl;
}

static void
write_error (std::string const& request_id, std::string const& code, std::string const& message)
{
	write_response (
		request_id,
		false,
		"error",
		"{\"code\":\"" + json_escape (code) + "\",\"message\":\"" + json_escape (message) + "\"}");
}

static std::string
error_code (std::string const& message)
{
	std::string prefix = "session_mismatch: ";
	if (!message.compare (0, prefix.size (), prefix)) {
		return "session_mismatch";
	}
	prefix = "stale_observation: ";
	if (!message.compare (0, prefix.size (), prefix)) {
		return "stale_observation";
	}
	prefix = "rollback_not_found: ";
	if (!message.compare (0, prefix.size (), prefix)) {
		return "rollback_not_found";
	}
	return "runtime_error";
}

static std::string
error_message (std::string const& message)
{
	std::string session_prefix = "session_mismatch: ";
	if (!message.compare (0, session_prefix.size (), session_prefix)) {
		return message.substr (session_prefix.size ());
	}
	std::string stale_prefix = "stale_observation: ";
	if (!message.compare (0, stale_prefix.size (), stale_prefix)) {
		return message.substr (stale_prefix.size ());
	}
	std::string rollback_prefix = "rollback_not_found: ";
	if (!message.compare (0, rollback_prefix.size (), rollback_prefix)) {
		return message.substr (rollback_prefix.size ());
	}
	return message;
}

static std::string
apply_commands (Session* session, pt::ptree const& commands)
{
	std::ostringstream result;
	bool first_result = true;

	result << "[";
	for (pt::ptree::const_iterator i = commands.begin (); i != commands.end (); ++i) {
		pt::ptree const& command = i->second;
		std::string op = command.get<std::string> ("op");
		if (!session) {
			throw std::runtime_error ("session_mismatch: no active session");
		}

		if (!first_result) {
			result << ",";
		}
		first_result = false;
		result << "{\"op\":\"" << json_escape (op) << "\"";

		if (op == "create_audio_track") {
			int input_channels = command.get<int> ("inputChannels", 1);
			int output_channels = command.get<int> ("outputChannels", 2);
			uint32_t count = command.get<uint32_t> ("count", 1);
			std::string name = command.get<std::string> ("name", "Audio");

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
				result << "{\"id\":\"" << json_escape ((*t)->id ().to_s ()) << "\""
				       << ",\"name\":\"" << json_escape ((*t)->name ()) << "\"}";
			}
			result << "]";
		} else if (op == "save_session") {
			if (session->save_state ("") != 0) {
				throw std::runtime_error ("save_session failed");
			}
			result << ",\"ok\":true";
		} else if (op == "import_audio") {
			std::string path = command.get<std::string> ("path");
			std::string track_id = command.get<std::string> ("trackId", "");
			std::string track_name = command.get<std::string> ("trackName", "");
			std::string region_name = command.get<std::string> ("regionName", "");
			std::string start_value = command.get<std::string> ("start", "0");
			std::string source_start_value = command.get<std::string> ("sourceStart", "0");
			std::string duration_value = command.get<std::string> ("duration", "");
			bool create_track = command.get<bool> ("createTrack", false);
			samplepos_t start = parse_position (*session, start_value);
			samplepos_t source_start = parse_position (*session, source_start_value);

			ImportStatus status;
			status.current = 0;
			status.total = 1;
			status.quality = SrcBest;
			status.freeze = false;
			status.replace_existing_source = false;
			status.split_midi_channels = false;
			status.import_markers = false;
			status.midi_track_name_source = SMFFileAndTrackName;
			status.all_done = false;
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
				region_name = runtime_region_name_from_path (path, sources.size () > 1, false);
			}
			region_name = unique_region_name (region_name);

			std::shared_ptr<AudioTrack> track = find_audio_track (*session, track_id, track_name);
			if (!track && create_track) {
				std::string new_track_name = track_name.empty () ? region_name : track_name;
				AudioTrackList tracks = session->new_audio_track (
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
			if (source_start < 0) {
				throw std::runtime_error ("import_audio sourceStart must be >= 0");
			}
			if (source_start >= sources[0]->length ().samples ()) {
				throw std::runtime_error ("import_audio sourceStart is beyond source length");
			}
			samplecnt_t region_length = sources[0]->length ().samples () - source_start;
			if (!duration_value.empty ()) {
				samplepos_t parsed_duration = parse_position (*session, duration_value);
				if (parsed_duration <= 0) {
					throw std::runtime_error ("import_audio duration must be > 0");
				}
				region_length = std::min<samplecnt_t> (parsed_duration, sources[0]->length ().samples () - source_start);
			}

			PBD::PropertyList plist;
			plist.add (Properties::start, timecnt_t (source_start, timepos_t (Temporal::AudioTime)));
			plist.add (Properties::length, timecnt_t (region_length, timepos_t (source_start)));
			plist.add (Properties::name, region_name);
			plist.add (Properties::layer, 0);
			plist.add (Properties::whole_file, source_start == 0 && region_length == sources[0]->length ().samples ());
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

			result << ",\"ok\":true,\"path\":\"" << json_escape (path) << "\""
			       << ",\"sourceCount\":" << sources.size ()
			       << ",\"trackId\":\"" << json_escape (track->id ().to_s ()) << "\""
			       << ",\"trackName\":\"" << json_escape (track->name ()) << "\""
			       << ",\"regionId\":\"" << json_escape (copy->id ().to_s ()) << "\""
			       << ",\"regionName\":\"" << json_escape (copy->name ()) << "\""
			       << ",\"start\":" << start
			       << ",\"sourceStart\":" << source_start
			       << ",\"length\":" << region_length;
		} else if (op == "place_audio") {
			std::string region_id = command.get<std::string> ("regionId", "");
			std::string region_name = command.get<std::string> ("regionName", "");
			std::string track_id = command.get<std::string> ("trackId", "");
			std::string track_name = command.get<std::string> ("trackName", "");
			std::string start_value = command.get<std::string> ("start");
			samplepos_t start = parse_position (*session, start_value);

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

			result << ",\"ok\":true,\"regionId\":\"" << json_escape (location.region->id ().to_s ()) << "\""
			       << ",\"regionName\":\"" << json_escape (location.region->name ()) << "\""
			       << ",\"trackId\":\"" << json_escape (target_track->id ().to_s ()) << "\""
			       << ",\"trackName\":\"" << json_escape (target_track->name ()) << "\""
			       << ",\"start\":" << start;
		} else {
			throw std::runtime_error ("unsupported operation: " + op);
		}
		result << "}";
	}
	result << "]";
	return result.str ();
}

int
main (int argc, char* argv[])
{
	Session* session = 0;
	std::string active_session_id;
	std::string active_session_dir;
	std::string active_session_name;
	uint32_t rollback_counter = 0;
	std::vector<RollbackPoint> rollback_points;
	bool stop = false;

	try {
		SessionUtils::init (false);
		std::string line;
		while (!stop && std::getline (std::cin, line)) {
			if (line.empty ()) {
				continue;
			}
			std::string request_id;
			try {
				std::istringstream input (line);
				pt::ptree request;
				pt::read_json (input, request);
				request_id = request.get<std::string> ("requestId", "");
				std::string method = request.get<std::string> ("method");
				pt::ptree empty_body;
				pt::ptree body = request.get_child ("body", empty_body);

				if (method == "runtime.start") {
					write_response (
						request_id,
						true,
						"response",
						"{\"protocolVersion\":0,\"capabilities\":[\"session.create\",\"session.observe\",\"commands.apply\",\"session.save\",\"render.preview\",\"session.rollback\",\"session.close\",\"runtime.stop\"]}");
				} else if (method == "runtime.stop") {
					write_response (request_id, true, "response", "{\"stopped\":true}");
					stop = true;
				} else if (method == "session.create") {
					std::string session_dir = body.get<std::string> ("sessionDir");
					std::string session_name = body.get<std::string> ("sessionName", default_session_name (session_dir));
					float sample_rate = body.get<float> ("sampleRate", 48000.0f);

					if (session) {
						SessionUtils::unload_session (session);
						session = 0;
					}
					session = SessionUtils::create_session (session_dir, session_name, sample_rate);
					if (!session) {
						throw std::runtime_error ("create_session failed");
					}
					active_session_dir = session_dir;
					active_session_name = session_name;
					rollback_points.clear ();
					active_session_id = "session_" + sha256_json (session_dir + ":" + session_name).substr (7, 16);
					write_response (
						request_id,
						true,
						"response",
						"{\"sessionId\":\"" + json_escape (active_session_id) + "\",\"sessionDir\":\"" + json_escape (session_dir) + "\",\"sessionName\":\"" + json_escape (session_name) + "\"}");
				} else if (method == "session.observe") {
					std::string session_id = body.get<std::string> ("sessionId");
					require_session_id (session_id, active_session_id);
					std::string observation = observe_session_json (*session);
					write_response (
						request_id,
						true,
						"response",
						"{\"sessionId\":\"" + json_escape (active_session_id) + "\",\"observationHash\":\"" + observe_hash (session) + "\",\"observation\":" + observation + "}");
				} else if (method == "session.save") {
					std::string session_id = body.get<std::string> ("sessionId");
					require_session_id (session_id, active_session_id);
					if (session->save_state ("") != 0) {
						throw std::runtime_error ("save_session failed");
					}
					write_response (request_id, true, "response", "{\"sessionId\":\"" + json_escape (active_session_id) + "\",\"saved\":true}");
				} else if (method == "session.close") {
					std::string session_id = body.get<std::string> ("sessionId");
					require_session_id (session_id, active_session_id);
					SessionUtils::unload_session (session);
					session = 0;
					std::string closed_session_id = active_session_id;
					active_session_id.clear ();
					active_session_dir.clear ();
					active_session_name.clear ();
					rollback_points.clear ();
					write_response (request_id, true, "response", "{\"sessionId\":\"" + json_escape (closed_session_id) + "\",\"closed\":true}");
				} else if (method == "render.preview") {
					std::string session_id = body.get<std::string> ("sessionId");
					require_session_id (session_id, active_session_id);
					std::string output_path = body.get<std::string> ("outputPath");
					RenderSettings settings;
					settings.sample_rate = body.get<int> ("sampleRate", session->nominal_sample_rate ());
					settings.sample_format = parse_sample_format (body.get<std::string> ("bitDepth", "16"));
					settings.normalize = body.get<bool> ("normalize", false);
					settings.broadcast = body.get<bool> ("broadcast", false);
					if (settings.sample_rate < 8000 || settings.sample_rate > 192000) {
						throw std::runtime_error ("invalid render sampleRate");
					}
					if (render_session (*session, output_path, settings) != 0) {
						throw std::runtime_error ("render failed");
					}
					write_response (
						request_id,
						true,
						"response",
						"{\"sessionId\":\"" + json_escape (active_session_id) + "\",\"outputPath\":\"" + json_escape (render_output_path (output_path)) + "\",\"sampleRate\":" + std::to_string (settings.sample_rate) + ",\"bitDepth\":\"" + json_escape (body.get<std::string> ("bitDepth", "16")) + "\"}");
				} else if (method == "commands.apply") {
					std::string session_id = body.get<std::string> ("sessionId");
					require_session_id (session_id, active_session_id);
					std::string expected_hash = body.get<std::string> ("expectedObservationHash", "");
					if (!expected_hash.empty () && expected_hash != observe_hash (session)) {
						throw std::runtime_error ("stale_observation: expectedObservationHash does not match active session");
					}
					if (session->save_state ("") != 0) {
						throw std::runtime_error ("snapshot save_state failed");
					}
					RollbackPoint rollback;
					rollback.rollback_id = "rollback_" + std::to_string (++rollback_counter);
					rollback.session_dir = active_session_dir;
					rollback.session_name = active_session_name;
					rollback.pre_hash = observe_hash (session);
					SnapshotInfo snapshot;
					snapshot.path = Glib::build_filename (Glib::path_get_dirname (active_session_dir), ".siann", "snapshots", rollback.rollback_id + "_before.tar.gz");
					capture_session_snapshot (active_session_dir, snapshot);
					rollback.snapshot_path = snapshot.path;
					rollback.snapshot_sha256 = snapshot.sha256;

					std::string results;
					try {
						results = apply_commands (session, body.get_child ("commands"));
					} catch (...) {
						SessionUtils::unload_session (session);
						session = 0;
						restore_session_snapshot (snapshot.path, active_session_dir);
						session = SessionUtils::load_session (active_session_dir, active_session_name);
						if (!session) {
							throw std::runtime_error ("failed to reload session after failed apply rollback");
						}
						throw;
					}
					rollback.post_hash = observe_hash (session);
					rollback_points.push_back (rollback);
					write_response (
						request_id,
						true,
						"response",
						"{\"sessionId\":\"" + json_escape (active_session_id) + "\",\"results\":" + results + ",\"observationHash\":\"" + rollback.post_hash + "\",\"rollback\":{\"rollbackId\":\"" + json_escape (rollback.rollback_id) + "\",\"snapshotPath\":\"" + json_escape (rollback.snapshot_path) + "\",\"snapshotSha256\":\"" + json_escape (rollback.snapshot_sha256) + "\",\"preObservationHash\":\"" + rollback.pre_hash + "\",\"postObservationHash\":\"" + rollback.post_hash + "\"}}");
				} else if (method == "session.rollback") {
					std::string session_id = body.get<std::string> ("sessionId");
					std::string rollback_id = body.get<std::string> ("rollbackId");
					require_session_id (session_id, active_session_id);
					std::vector<RollbackPoint>::const_iterator rollback = rollback_points.end ();
					for (std::vector<RollbackPoint>::const_iterator r = rollback_points.begin (); r != rollback_points.end (); ++r) {
						if (r->rollback_id == rollback_id) {
							rollback = r;
							break;
						}
					}
					if (rollback == rollback_points.end ()) {
						throw std::runtime_error ("rollback_not_found: unknown rollbackId");
					}
					SessionUtils::unload_session (session);
					session = 0;
					restore_session_snapshot (rollback->snapshot_path, rollback->session_dir);
					session = SessionUtils::load_session (rollback->session_dir, rollback->session_name);
					if (!session) {
						throw std::runtime_error ("session.rollback failed to reload session");
					}
					active_session_dir = rollback->session_dir;
					active_session_name = rollback->session_name;
					rollback_points.erase (rollback, rollback_points.end ());
					write_response (
						request_id,
						true,
						"response",
						"{\"sessionId\":\"" + json_escape (active_session_id) + "\",\"rollbackId\":\"" + json_escape (rollback_id) + "\",\"rolledBack\":true,\"observationHash\":\"" + observe_hash (session) + "\"}");
				} else {
					throw std::runtime_error ("unsupported method: " + method);
				}
			} catch (std::exception const& e) {
				std::string message = e.what ();
				write_error (request_id, error_code (message), error_message (message));
			}
		}

		if (session) {
			SessionUtils::unload_session (session);
		}
		SessionUtils::cleanup ();
		return 0;
	} catch (std::exception const& e) {
		if (session) {
			SessionUtils::unload_session (session);
		}
		SessionUtils::cleanup ();
		std::cerr << "siann_runtime: " << e.what () << std::endl;
		return 1;
	}
}
