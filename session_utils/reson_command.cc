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

#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <glibmm.h>

#include "ardour/audioregion.h"
#include "ardour/audio_track.h"
#include "ardour/import_status.h"
#include "ardour/midi_track.h"
#include "ardour/playlist.h"
#include "ardour/presentation_info.h"
#include "ardour/region.h"
#include "ardour/region_factory.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/source.h"
#include "ardour/utils.h"

#include "common.h"

using namespace ARDOUR;
using namespace SessionUtils;
namespace pt = boost::property_tree;

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
			    << ",\"hidden\":" << (route->is_hidden () ? "true" : "false")
			    << "}";
		}
	}

	out << "]}";
	return out.str ();
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
	std::cout << "  create_session, open_session, create_audio_track, import_audio, save_session, observe_session\n\n";
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

	result << "{\"schemaVersion\":\"reson.result.v0\",\"results\":[";

	try {
		pt::ptree const& commands = root.get_child ("commands");

		for (pt::ptree::const_iterator i = commands.begin (); i != commands.end (); ++i) {
			pt::ptree const& command = i->second;
			std::string      op      = command.get<std::string> ("op");

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

				result << ",\"ok\":true,\"sessionDir\":\"" << json_escape (session_dir) << "\""
				       << ",\"sessionName\":\"" << json_escape (session_name) << "\"";

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

				result << ",\"ok\":true,\"path\":\"" << json_escape (path) << "\""
				       << ",\"sourceCount\":" << sources.size ()
				       << ",\"trackId\":\"" << json_escape (track->id ().to_s ()) << "\""
				       << ",\"trackName\":\"" << json_escape (track->name ()) << "\""
				       << ",\"regionId\":\"" << json_escape (copy->id ().to_s ()) << "\""
				       << ",\"regionName\":\"" << json_escape (copy->name ()) << "\""
				       << ",\"start\":" << start;

			} else if (op == "save_session") {
				require_session (session, op);

				if (session->save_state ("") != 0) {
					throw std::runtime_error ("save_session failed");
				}
				result << ",\"ok\":true";

			} else if (op == "observe_session") {
				require_session (session, op);

				result << ",\"ok\":true,\"observation\":" << observe_session_json (*session);

			} else {
				throw std::runtime_error ("unsupported operation: " + op);
			}

			result << "}";
		}

	} catch (std::exception const& e) {
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
