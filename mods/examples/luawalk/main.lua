-- main.lua - reads entities every turn through the read-only view.
--
-- It reports from on_turn rather than from on_level_end, because a smoke run
-- that starts a level and quits never leaves it: a summary hung off the level
-- end would be a metric the gates could never collect.
local most, kinds, reported = 0, {}, 0

local function shapes()
  local n = 0
  for _ in pairs(kinds) do n = n + 1 end
  return n
end

pop.on_turn("after", function()
  local n = pop.entity_count()
  if n > most then most = n end
  for i = 0, n - 1 do
    local e = pop.entity(pop.entity_slot(i))     -- a real decode, per entity
    kinds[e.kind] = (kinds[e.kind] or 0) + 1
  end
  -- Report the first time there is anything to report, then every 100 turns,
  -- so the log carries the metric whatever the run's length turns out to be.
  local turn = pop.simulation_turn()
  if most > 0 and (reported == 0 or turn - reported >= 100) then
    reported = turn
    pop.log("luawalk saw " .. most .. " entities of " .. shapes() ..
            " kinds, turn " .. turn)
  end
end)

pop.on_level_end(function()
  pop.log("luawalk saw " .. most .. " entities of " .. shapes() ..
          " kinds, turn " .. pop.simulation_turn())
end)
