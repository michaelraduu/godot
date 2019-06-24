/*************************************************************************/
/*  lws_server.cpp                                                       */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2019 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2019 Godot Engine contributors (cf. AUTHORS.md)    */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#ifndef JAVASCRIPT_ENABLED

#include "wsl_server.h"
#include "core/os/os.h"
#include "core/project_settings.h"

bool WSLServer::PendingPeer::_parse_request(String &r_key) {
	Vector<String> psa = request.trim_suffix("\r\n\r\n").split("\r\n");
	int len = psa.size();
	if (len < 4) {
		ERR_EXPLAIN("Not enough response headers.");
		ERR_FAIL_V(false);
	}

	Vector<String> req = psa[0].split(" ", false);
	if (req.size() < 2) {
		ERR_EXPLAIN("Invalid protocol or status code.");
		ERR_FAIL_V(false);
	}
	// Wrong protocol
	if (req[0] != "GET" || req[2] != "HTTP/1.1") {
		ERR_EXPLAIN("Invalid method or HTTP version.");
		ERR_FAIL_V(false);
	}

	Map<String, String> headers;
	for (int i = 1; i < len; i++) {
		Vector<String> header = psa[i].split(":", false, 1);
		if (header.size() != 2) {
			ERR_EXPLAIN("Invalid header -> " + psa[i]);
			ERR_FAIL_V(false);
		}
		String name = header[0].to_lower();
		String value = header[1].strip_edges();
		if (headers.has(name))
			headers[name] += "," + value;
		else
			headers[name] = value;
	}
#define _WLS_CHECK(NAME, VALUE)                                                                      \
	ERR_EXPLAIN("Missing or invalid header '" + String(NAME) + "'. Expected value '" + VALUE + "'"); \
	ERR_FAIL_COND_V(!headers.has(NAME) || headers[NAME].to_lower() != VALUE, false);
#define _WLS_CHECK_EX(NAME)                                \
	ERR_EXPLAIN("Missing header '" + String(NAME) + "'."); \
	ERR_FAIL_COND_V(!headers.has(NAME), false);
	_WLS_CHECK("upgrade", "websocket");
	_WLS_CHECK("sec-websocket-version", "13");
	_WLS_CHECK_EX("sec-websocket-key");
	_WLS_CHECK_EX("connection");
#undef _WLS_CHECK_EX
#undef _WLS_CHECK
	r_key = headers["sec-websocket-key"];
	return true;
}

Error WSLServer::PendingPeer::do_handshake() {
	if (OS::get_singleton()->get_ticks_msec() - time > WSL_SERVER_TIMEOUT)
		return ERR_TIMEOUT;
	if (!has_request) {
		uint8_t byte = 0;
		int read = 0;
		while (true) {
			Error err = connection->get_partial_data(&byte, 1, read);
			if (err != OK) // Got an error
				return FAILED;
			else if (read != 1) // Busy, wait next poll
				return ERR_BUSY;
			request += byte;

			if (request.size() > WSL_MAX_HEADER_SIZE) {
				ERR_EXPLAIN("Response headers too big");
				ERR_FAIL_V(ERR_OUT_OF_MEMORY);
			}
			if (request.ends_with("\r\n\r\n")) {
				if (!_parse_request(key)) {
					return FAILED;
				}
				String r = "HTTP/1.1 101 Switching Protocols\r\n";
				r += "Upgrade: websocket\r\n";
				r += "Connection: Upgrade\r\n";
				r += "Sec-WebSocket-Accept: " + WSLPeer::compute_key_response(key) + "\r\n";
				r += "\r\n";
				response = r.utf8();
				has_request = true;
				WARN_PRINTS("Parsed, " + key);
				break;
			}
		}
	}
	if (has_request && response_sent < response.size() - 1) {
		int sent = 0;
		Error err = connection->put_partial_data((const uint8_t *)response.get_data() + response_sent, response.size() - response_sent - 1, sent);
		if (err != OK) {
			return err;
		}
		response_sent += sent;
	}
	if (response_sent < response.size() - 1)
		return ERR_BUSY;
	return OK;
}

Error WSLServer::listen(int p_port, PoolVector<String> p_protocols, bool gd_mp_api) {
	ERR_FAIL_COND_V(is_listening(), ERR_ALREADY_IN_USE);

	_is_multiplayer = gd_mp_api;
	_server->listen(p_port);

	return OK;
}

void WSLServer::poll() {

	List<int> remove_ids;
	for (Map<int, Ref<WebSocketPeer> >::Element *E = _peer_map.front(); E; E = E->next()) {
		Ref<WSLPeer> peer = (WSLPeer *)E->get().ptr();
		peer->poll();
		if (!peer->is_connected_to_host()) {
			_on_disconnect(E->key(), peer->close_code != -1);
			remove_ids.push_back(E->key());
		}
	}
	for (List<int>::Element *E = remove_ids.front(); E; E = E->next()) {
		_peer_map.erase(E->get());
	}
	remove_ids.clear();

	List<Ref<PendingPeer> > remove_peers;
	for (List<Ref<PendingPeer> >::Element *E = _pending.front(); E; E = E->next()) {
		Ref<PendingPeer> ppeer = E->get();
		Error err = ppeer->do_handshake();
		if (err == ERR_BUSY) {
			continue;
		} else if (err != OK) {
			remove_peers.push_back(ppeer);
			continue;
		}
		// Creating new peer
		int32_t id = _gen_unique_id();

		WSLPeer::PeerData *data = memnew(struct WSLPeer::PeerData);
		data->obj = this;
		data->conn = ppeer->connection;
		data->is_server = true;
		data->id = id;

		Ref<WSLPeer> ws_peer = memnew(WSLPeer);
		ws_peer->make_context(data, _in_buf_size, _in_pkt_size, _out_buf_size, _out_pkt_size);

		_peer_map[id] = ws_peer;
		remove_peers.push_back(ppeer);
		_on_connect(id, "");
	}
	for (List<Ref<PendingPeer> >::Element *E = remove_peers.front(); E; E = E->next()) {
		_pending.erase(E->get());
	}
	remove_peers.clear();

	if (!_server->is_listening())
		return;

	while (_server->is_connection_available()) {
		Ref<StreamPeer> conn = _server->take_connection();
		if (is_refusing_new_connections())
			continue; // Conn will go out-of-scope and be closed.

		Ref<PendingPeer> peer = memnew(PendingPeer);
		peer->connection = conn;
		peer->time = OS::get_singleton()->get_ticks_msec();
		_pending.push_back(peer);
	}
}

bool WSLServer::is_listening() const {
	return _server->is_listening();
}

int WSLServer::get_max_packet_size() const {
	return (1 << _out_buf_size) - PROTO_SIZE;
}

void WSLServer::stop() {
	_server->stop();
	for (Map<int, Ref<WebSocketPeer> >::Element *E = _peer_map.front(); E; E = E->next()) {
		Ref<WSLPeer> peer = (WSLPeer *)E->get().ptr();
		peer->close_now();
	}
	_pending.clear();
	_peer_map.clear();
}

bool WSLServer::has_peer(int p_id) const {
	return _peer_map.has(p_id);
}

Ref<WebSocketPeer> WSLServer::get_peer(int p_id) const {
	ERR_FAIL_COND_V(!has_peer(p_id), NULL);
	return _peer_map[p_id];
}

IP_Address WSLServer::get_peer_address(int p_peer_id) const {
	ERR_FAIL_COND_V(!has_peer(p_peer_id), IP_Address());

	return _peer_map[p_peer_id]->get_connected_host();
}

int WSLServer::get_peer_port(int p_peer_id) const {
	ERR_FAIL_COND_V(!has_peer(p_peer_id), 0);

	return _peer_map[p_peer_id]->get_connected_port();
}

void WSLServer::disconnect_peer(int p_peer_id, int p_code, String p_reason) {
	ERR_FAIL_COND(!has_peer(p_peer_id));

	get_peer(p_peer_id)->close(p_code, p_reason);
}

Error WSLServer::set_buffers(int p_in_buffer, int p_in_packets, int p_out_buffer, int p_out_packets) {
	ERR_EXPLAIN("Buffers sizes can only be set before listening or connecting");
	ERR_FAIL_COND_V(_server->is_listening(), FAILED);

	_in_buf_size = nearest_shift(p_in_buffer - 1) + 10;
	_in_pkt_size = nearest_shift(p_in_packets - 1);
	_out_buf_size = nearest_shift(p_out_buffer - 1) + 10;
	_out_pkt_size = nearest_shift(p_out_packets - 1);
	return OK;
}

WSLServer::WSLServer() {
	_in_buf_size = nearest_shift((int)GLOBAL_GET(WSS_IN_BUF) - 1) + 10;
	_in_pkt_size = nearest_shift((int)GLOBAL_GET(WSS_IN_PKT) - 1);
	_out_buf_size = nearest_shift((int)GLOBAL_GET(WSS_OUT_BUF) - 1) + 10;
	_out_pkt_size = nearest_shift((int)GLOBAL_GET(WSS_OUT_PKT) - 1);
	_server.instance();
}

WSLServer::~WSLServer() {
	stop();
}

#endif // JAVASCRIPT_ENABLED
