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
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <glibmm.h>

#include "ardour/audio_track.h"
#include "ardour/midi_track.h"
#include "ardour/presentation_info.h"
#include "ardour/route.h"
#include "ardour/session.h"

#include "common.h"

using namespace ARDOUR;
using namespace SessionUtils;
namespace pt = boost::property_tree;

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
	std::cout << "  create_session, open_session, create_audio_track, save_session, observe_session\n\n";
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
