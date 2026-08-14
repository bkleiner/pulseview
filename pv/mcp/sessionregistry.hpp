/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef PULSEVIEW_PV_MCP_SESSIONREGISTRY_HPP
#define PULSEVIEW_PV_MCP_SESSIONREGISTRY_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace pv {

class Session;

namespace mcp {

class SessionRegistry
{
public:
	struct Entry {
		uint64_t id;
		uint64_t generation;
		bool active;
		std::shared_ptr<Session> session;
	};

	SessionRegistry();

	uint64_t add(const std::shared_ptr<Session>& session);
	void remove(const Session *session);
	void set_active(const Session *session);
	void changed(const Session *session);

	std::vector<Entry> entries() const;
	bool resolve(uint64_t id, Entry& entry) const;
	bool active(Entry& entry) const;

private:
	struct StoredEntry {
		uint64_t id;
		uint64_t generation;
		std::weak_ptr<Session> session;
	};

	uint64_t next_id_;
	uint64_t active_id_;
	std::map<const Session*, StoredEntry> entries_;
};

} // namespace mcp
} // namespace pv

#endif // PULSEVIEW_PV_MCP_SESSIONREGISTRY_HPP
