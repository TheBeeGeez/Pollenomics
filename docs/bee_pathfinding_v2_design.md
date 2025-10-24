# Bee Pathfinding v2 — Flow-Field Design Doc

## Context

The current navigation stack in `src/sim/bee_path.c` performs per-bee steering by probing for
collision-free lines of sight, leaning on heuristics around the hive entrance and velocity
alignment. While this avoids heavy graph search per bee, it does not exploit the structure of the
hexagonal world (`HexWorld`) or the extensive per-tile data already available on `HexTile`
(`base_cost`, `passable`, `hive_storage_slot`, etc.). The existing API, `bee_path_plan`, returns a
`BeePathPlan` populated with a direction vector and optional intermediate waypoint, and is invoked
from the simulation loop in `src/sim/sim.c` via the `SimState` arrays (`x`, `y`, `target_pos_x`,
`mode`, `intent`, ...).

Bee Pathfinding v2 replaces that heuristic layer with precomputed flow-fields over the hex map. The
pathfinding system emits per-tile "policy" arrows that bees can sample each frame. This design keeps
rendering, bee state machines, and physics untouched while providing higher-quality global routing
with tight CPU budgets.

## Goals

* Reuse the existing `HexWorld` tile graph to provide deterministic navigation toward multiple goal
  classes (entrances, unload/storage cells, viable flowers).
* Expose a stable, lightweight query API that the sim loop can call instead of `bee_path_plan`,
  returning a world-space direction derived from the flow-field.
* Support dynamic costs influenced by congestion (per-tile flow capacity), hazards, wind, and user
  edits, with tight control over recomputation time.
* Supply debugging overlays (distance heatmaps, arrow fields) that can be toggled in the existing
  renderer for introspection without changing bee rendering.

## File Layout

New pathfinding code lives under `include/path/` and `src/path/`, isolating the flow-field logic from
simulation and rendering modules.

```
include/
  path/path.h                 // public queries and goal definitions
  path/path_cost.h            // dynamic cost controls and dirty markers
  path/path_fields.h          // read-only field views for overlays/tests
  path/path_scheduler.h       // recompute cadence and test hooks

src/path/
  path_core.c                 // graph prep, goal set extraction, handles
  path_cost.c                 // effective tile costs and dirty tracking
  path_fields.c               // multi-source Dijkstra solver & double buffers
  path_scheduler.c            // time slicing, per-goal budgets, dirties
  path_debug.c                // overlay generation for renderer/UI
```

No other subsystem includes these `.c` files directly; they interact via the headers above.

## Public API

### `include/path/path.h`

* `typedef enum PathGoal { ... } PathGoal;` enumerates goal sets that matter to bees. Initial values
  cover hive entrances (`HEX_TERRAIN_HIVE_ENTRANCE`), unload/storage tiles (`HexTile::hive_storage_slot`
  >= 0), and viable flower tiles (`hex_world_tile_is_floral`).
* Lifecycle functions: `bool path_init(const HexWorld *world, const Params *params);` and
  `void path_shutdown(void);` mirror how `sim_init` and `sim_shutdown` manage `SimState` and
  `HexWorld` lifetimes.
* Per-frame update: `void path_update(const HexWorld *world, const Params *params, float dt);` gives
  the scheduler a budgeted timeslice, similar to how `sim_update` advances bee state.
* Bee queries: `bool path_query_direction(PathGoal goal, size_t tile_index, Vec2 *dir_world_out);`
  returns a normalized direction vector centered on the tile retrieved by
  `hex_world_tile_from_world`. A variant `path_query_direction_biased` accepts a
  `PathBias` struct (containing nearby flower desirability) so scouts can blend the global policy
  with opportunistic local pulls.
* UI helpers: `const size_t *path_goals(PathGoal goal, int *count_out);` exposes the current goal
  indices for overlays/debug panels.

### `include/path/path_cost.h`

This header lets other systems publish cost adjustments without understanding the solver.

* Coefficient setters such as `path_cost_set_coeffs(float alpha, float beta, float gamma);` map onto
  user-tunable fields that already exist in `Params` (`bee_speed_mps`, `bee_seek_accel`, etc.).
* Dirties: `void path_cost_mark_dirty(size_t tile_index);` and
  `void path_cost_mark_many_dirty(const size_t *indices, int count);` are used by congestion updates
  in `sim_update` (when `SimState::path_waypoint_x` shows tile crossings), hive editing tools in the
  UI, or weather systems.

### `include/path/path_fields.h`

Renderer and tooling modules (`src/render`, `src/ui`) can pull raw buffers to visualize the flow
fields.

* `const float *path_field_dist(PathGoal goal);` exposes per-tile distances.
* `const uint8_t *path_field_next(PathGoal goal);` encodes which neighbor (0–5) minimizes cost.
* `uint32_t path_field_stamp(PathGoal goal);` increments whenever a field fully recomputes so
  overlays know when to refresh vertex buffers.

### `include/path/path_scheduler.h`

* `void path_sched_set_budget_ms(float per_frame_ms);` lets the app expose a slider alongside
  existing params (see `src/ui/ui_params.c`).
* `void path_sched_set_goal_rate(PathGoal goal, float hz);` tunes cadence per goal class (entrances
  update more frequently than flowers).
* `void path_sched_force_full_recompute(PathGoal goal);` is compiled under test builds so automated
  checks in `tests/` can assert convergence.

## Responsibilities

### Graph Assembly (`src/path/path_core.c`)

* Cache axial neighbor indices for every tile in `HexWorld::tiles`. Use the existing axial bounds
  (`q_min`, `q_max`, `r_min`, `r_max`) to clamp out-of-bounds neighbors and skip
  `HexTile::passable == false` (walls, water).
* Build goal sets directly from tile data:
  * Entrances: tiles with `terrain == HEX_TERRAIN_HIVE_ENTRANCE`.
  * Unload: tiles where `HexTile::hive_storage_slot >= 0` or `hex_world_hive_preferred_unload`
    resolves.
  * Flowers: indices returned by `SimState::floral_tile_indices` filtered by positive
    `HexTile::nectar_stock`.
* Provide small opaque handles so other modules refer to goal sets without holding pointers.

### Cost Aggregation (`src/path/path_cost.c`)

* Maintain per-tile arrays for the cost components: base terrain (`HexTile::base_cost`), congestion,
  hazards, wind. Congestion comes from an EMA computed in `sim_update` using the bees' tile indices;
  store the smoothed density and translate to penalties via
  `penalty = max(0, density / flow_capacity - 1)^2` (`flow_capacity` already lives on `HexTile`).
* Track a byte `dirty_flags[tile_index]`. Whenever any component changes beyond a threshold, set the
  flag and enqueue the tile for the scheduler.
* Compute `effective_cost[index] = base + alpha * congestion + beta * wind + gamma * hazard` on
  demand when the solver relaxes an edge.

### Flow-Field Solver (`src/path/path_fields.c`)

* For each `PathGoal`, allocate double-buffered arrays `dist[2][tile_count]` and
  `next[2][tile_count]`. Buffer indices swap only after the solver empties its priority queue.
* Store the ongoing Dijkstra frontier in a binary heap of `(tile_index, distance)` plus visited flags
  to resume across frames.
* On initialization, push all tiles in the goal set with distance 0. Relax neighbors using axial
  offsets. Edge traversal cost is `distance[u] + move_cost(u, v)`, where `move_cost` adds the
  effective cost of entering `v` and any directional wind penalty.
* Incremental updates: when `dirty_flags` indicate a change, reinsert affected tiles with `+∞`
  distance so the frontier reflows locally instead of recomputing the entire map.
* When the queue runs dry, copy results into the inactive buffers, increment `stamp`, and atomically
  swap the exposed pointers.

### Scheduler (`src/path/path_scheduler.c`)

* Track per-goal cadence timers and a shared millisecond budget (default 1–2 ms per frame). Use the
  app's delta time from `path_update` to decrement timers, similar to how the sim throttles floral
  regeneration (`SimState::floral_clock_sec`).
* Each `path_update` call repeatedly picks the stalest goal whose timer expired and advances its
  solver for as many pop-relax steps as the budget allows. Abort when budget consumed or the queue is
  empty.
* When a goal finishes, clear its timer, rotate it to the back of a small priority queue, and perform
  the buffer swap described above.

### Debug Overlays (`src/path/path_debug.c`)

* Sample one arrow per N tiles (configurable) based on `path_field_next`. Convert axial directions to
  world coordinates via `hex_world_axial_to_world` and store them in a `RenderView`-compatible vertex
  buffer.
* Provide heatmap textures derived from `path_field_dist` so the renderer can toggle them like the
  nectar heatmap in `hex_world_apply_palette`.

## Simulation Integration

`src/sim/sim.c` replaces calls to `bee_path_plan` with the new path API.

1. Convert bee world position to a tile index using `hex_world_tile_from_world`. If the bee is
   outside the grid, steer back toward the closest valid center (existing logic already clamps
   positions; reuse `hex_world_axial_round`).
2. Select a `PathGoal` based on `SimState::intent[index]` and `SimState::inside_hive_flag[index]`:
   * Returning bees → `PATH_GOAL_ENTRANCE`.
   * Bees carrying nectar in the hive → `PATH_GOAL_UNLOAD`.
   * Explorers/scouts → `PATH_GOAL_FLOWERS_NEAR`.
3. Call `path_query_direction(goal, tile_index, &dir)`. Blend `dir` with the existing steering vector
   (jitter, avoidance) before integrating velocity using `bee_seek_accel` and `bee_speed_mps`.
4. If `path_query_direction` fails (field not ready or tile unreachable), fall back to the current
   heuristic in `bee_path_plan` but log a throttled warning for telemetry.

`bee_path_plan` remains temporarily for fallback and to minimize disruption to other behaviors while
Bee Pathfinding v2 rolls out.

## Dynamic Inputs

* **Congestion**: `sim_update` already tracks bee positions; reuse those loops to increment a per-tile
  crossing counter each tick. Every 0.1–0.2 s, convert counts to flow rates, smooth with an EMA, and
  call `path_cost_mark_dirty` for tiles whose density changed more than 5%.
* **Terrain editing**: When the UI modifies hive walls or toggles deposit slots, mark the affected
  tiles dirty and rebuild goal sets if a tile switches between passable/impassable states.
* **Flower viability**: `SimState::floral_tile_indices` contains current floral tiles. When a tile's
  `HexTile::nectar_stock` crosses a viability threshold, update the goal set for
  `PATH_GOAL_FLOWERS_NEAR` and force a recompute via the scheduler.

## Parameters & Defaults

Expose new knobs in `Params` (hooked up through `src/config` and `src/ui`):

* `path_budget_ms_per_frame` default 1.5 ms (`path_sched_set_budget_ms`).
* Goal cadences: entrances/unload 10 Hz, flowers 3 Hz (`path_sched_set_goal_rate`).
* Cost weights: `alpha_congestion = 1.0`, `beta_wind = 0.0`, `gamma_hazard = 2.0`.
* Base terrain costs seeded from `HexTile::base_cost` but clamped so hive walls remain impassable.

## Testing & Telemetry

* Functional smoke tests: painting a wall in the hive UI (`src/ui`) should reroute bees within 0.5 s;
  reducing `HexTile::base_cost` on an entrance should increase flow through it.
* Performance counters: record per-goal relax steps/sec and queue sizes in the app telemetry panel
  (`SimState::log_*` style). The scheduler should never exceed its budget; expose actual usage in the
  debug HUD.
* Robustness: when a bee stands on a tile with `next == 255`, steer toward the goal center using
  existing fallback logic and log once per second.

## Rollout Plan

1. **PR1 – Graph & flow-field skeleton:** Implement core neighbors, goal extraction for hive
   entrances, full Dijkstra recompute, and a basic overlay. Wire up `path_init`, `path_update`, and
   `path_query_direction` for entrances only.
2. **PR2 – Scheduler & buffering:** Add incremental stepping, per-goal cadences, buffer swapping, and
   stamps. Integrate with app parameters for the per-frame budget.
3. **PR3 – Dynamic costs:** Implement congestion tracking in `sim_update`, hazard/wind fields, dirty
   propagation, and incremental relaxation.
4. **PR4 – Flower goals & biases:** Populate flower goal sets, add the biased query hook, and replace
   most `bee_path_plan` calls.
5. **PR5 – Polish & observability:** Optional wind integration, UI sliders, HUD stats, and extended
   tests.

