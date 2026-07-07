/*
 * Copyright (C) 2026 Gary Hsieh <romanticamaj@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <cstdlib>
#include <cmath>
#include <ctime>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <glibmm.h>

#include "ardour/audio_track.h"
#include "ardour/audioregion.h"
#include "ardour/file_source.h"
#include "ardour/midi_track.h"
#include "ardour/playlist.h"
#include "ardour/presentation_info.h"
#include "ardour/region.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/source.h"
#include "ardour/track.h"

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
						"{\"protocolVersion\":0,\"capabilities\":[\"session.create\",\"session.observe\",\"commands.apply\",\"session.save\",\"runtime.stop\"]}");
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
				} else if (method == "commands.apply") {
					std::string session_id = body.get<std::string> ("sessionId");
					require_session_id (session_id, active_session_id);
					std::string expected_hash = body.get<std::string> ("expectedObservationHash", "");
					if (!expected_hash.empty () && expected_hash != observe_hash (session)) {
						throw std::runtime_error ("stale_observation: expectedObservationHash does not match active session");
					}
					std::string results = apply_commands (session, body.get_child ("commands"));
					write_response (
						request_id,
						true,
						"response",
						"{\"sessionId\":\"" + json_escape (active_session_id) + "\",\"results\":" + results + ",\"observationHash\":\"" + observe_hash (session) + "\"}");
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
