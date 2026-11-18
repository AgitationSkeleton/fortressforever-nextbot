-- ff-nextbot census (spike). Loaded through sv_globalluascript after the map's own script.
-- Reads the map's Lua objects and reports them to the bot plugin through LogLuaEvent
-- ("luaevent" game event). It never changes game state; every step runs under pcall.

ffnb = ffnb or {}
ffnb.version = 1

local function say(s) pcall(print, "[ffnb] " .. s) end

-- reverse map: table -> global name (for naming ancestors)
local function global_names()
	local names = {}
	for k, v in pairs(_G) do
		if type(k) == "string" and type(v) == "table" and names[v] == nil then names[v] = k end
	end
	return names
end

-- "blue_flag<baseflag<info_ff_script<baseclass"
local function ancestry(t, names)
	local chain, m, n = {}, getmetatable(t), 0
	while m ~= nil and n < 16 do
		table.insert(chain, names[m] or "?")
		m = getmetatable(m)
		n = n + 1
	end
	return table.concat(chain, "<")
end

-- IMPORTANT: userid/userid2 must stay 0. FF's event logger (ff_eventlog.cpp) treats a non-zero
-- value as a player index and dereferences it unchecked, which crashes the server. The entity
-- index travels in key0/value0 ("ent") instead.
local function emit(idx, name, k1, v1, k2, v2)
	pcall(LogLuaEvent, 0, 0, name, "ent", tostring(idx or 0), k1 or "", tostring(v1 or ""), k2 or "", tostring(v2 or ""))
end

-- teams allowed to touch an object (touchflags), as a bitmask of 1<<team; -1 = not restricted
local function touchmask(t)
	local tf = t.touchflags
	if type(tf) ~= "table" then return -1 end
	local m, any = 0, false
	for _, f in pairs(tf) do
		if f == AllowFlags.kBlue then m = m + 4; any = true
		elseif f == AllowFlags.kRed then m = m + 8; any = true
		elseif f == AllowFlags.kYellow then m = m + 16; any = true
		elseif f == AllowFlags.kGreen then m = m + 32; any = true end
	end
	if not any then return -1 end
	return m
end

-- backpack contents: "pack:h=25,a=50,am=60,g=2"
local function packinfo(t)
	if type(t.health) ~= "number" and type(t.armor) ~= "number" then return nil end
	local n = function(v) return type(v) == "number" and v or 0 end
	return "pack:h=" .. n(t.health) .. ",a=" .. n(t.armor) .. ",am=" .. (n(t.shells) + n(t.nails) + n(t.rockets) + n(t.cells))
		.. ",g=" .. (n(t.gren1) + n(t.gren2) + n(t.grenades))
end

function ffnb.census()
	emit(0, "ffnb_census_begin", "", "", "", "")
	ffnb.watch = {}
	local names = global_names()
	local count = 0
	for k, t in pairs(_G) do
		if type(k) == "string" and type(t) == "table" and getmetatable(t) ~= nil then
			local ok, ents = pcall(GetEntitiesByName, k)
			if ok and ents ~= nil then
				for _, e in pairs(ents) do
					local idx = 0
					pcall(function() idx = e:GetId() end)
					local cls = ""
					pcall(function() cls = e:GetClassName() end)
					emit(idx, "ffnb_obj", "name", k, "base", ancestry(t, names) .. "|" .. cls)
					-- remember objectives so ffnb.poll can report Remove/Restore (attack/defend phases)
					if t.botgoaltype == 5 or t.botgoaltype == 6 or t.item ~= nil then
						table.insert(ffnb.watch, { idx = idx, ent = e, removed = nil, cp = t.cp_number, owner = nil })
					end
					local item = ""
					pcall(function()
						if type(t.item) == "table" then
							local parts = {}
							for _, v in pairs(t.item) do table.insert(parts, tostring(v)) end
							item = table.concat(parts, ",")
						elseif t.item ~= nil then
							item = tostring(t.item)
						end
					end)
					if item == "" then
						local ok2, pi = pcall(packinfo, t)
						if ok2 and pi then item = pi end
					end
					local tm = -1
					pcall(function() tm = touchmask(t) end)
					emit(idx, "ffnb_objf", "team", tostring(t.team) .. "|" .. tostring(t.botgoaltype) .. "|" .. tostring(tm), "item", item)
					count = count + 1
				end
			end
		end
	end
	emit(0, "ffnb_census_done", "objects", count, "map", GetMapName and GetMapName() or "")
	say("census: " .. count .. " objects")
end

-- the plugin asks for a fresh census (e.g. after a hot reload) by changing ffnb_census_seq
ffnb.seq = nil
-- objective state (IsRemoved) changes -> ffnb_state events
function ffnb.watchstate()
	for _, w in pairs(ffnb.watch or {}) do
		local ok, removed = pcall(function()
			local e = CastToInfoScript(w.ent) or CastToTriggerScript(w.ent)
			return e:IsRemoved()
		end)
		if ok and removed ~= w.removed then
			w.removed = removed
			emit(w.idx, "ffnb_state", "state", removed and "removed" or "active", "", "")
		end
		-- command point ownership (base_cp: command_points[n].defending_team)
		if w.cp ~= nil and type(command_points) == "table" and type(command_points[w.cp]) == "table" then
			local owner = command_points[w.cp].defending_team
			if owner ~= w.owner then
				w.owner = owner
				emit(w.idx, "ffnb_owner", "team", tostring(owner), "", "")
			end
		end
	end
end

function ffnb.poll()
	pcall(ffnb.watchstate)
	local ok, seq = pcall(GetConvar, "ffnb_census_seq")
	if not ok or seq == nil or seq < 0 then return end
	if ffnb.seq ~= nil and seq ~= ffnb.seq then
		pcall(ffnb.census)
	end
	ffnb.seq = seq
end

-- wrap startup(): run the map's own first, then the census
local map_startup = startup
function startup(...)
	-- run the map's startup first; if it fails (some map scripts do), still take the census and
	-- then re-raise the map's error unchanged so FF reports it exactly as before
	local mok, r = true, nil
	if map_startup ~= nil then mok, r = pcall(map_startup, ...) end
	local ok, err = pcall(ffnb.census)
	if not ok then say("census failed: " .. tostring(err)) end
	pcall(AddScheduleRepeating, "ffnb_poll", 1.0, ffnb.poll)
	if not mok then error(r, 0) end
	return r
end

say("loaded")
