# SPDX-License-Identifier: GPL-2.0-only
import re;
import itertools
import copy
import subprocess

# Fetch the FSM table from the source
with open("drivers/vfio/vfio.c", "rt") as F:
    g = re.search(
        r"static const u8 vfio_from_fsm_table\[VFIO_MIG_FSM_MAX_STATE\]\[VFIO_MIG_FSM_MAX_STATE\] = {(.*?)};",
        F.read(), re.MULTILINE | re.DOTALL)
    code = g.group(1).strip()

# Parse it
all_arcs = {}
for ln in code.splitlines():
    ln = ln.strip()
    g = re.match(r"\[VFIO_DEVICE_STATE_(.*)\] = {", ln)
    if g:
        cur_fsm = g.group(1)
        continue
    g = re.match(r"\[VFIO_DEVICE_STATE_(.*)\] = VFIO_DEVICE_STATE_(.*),", ln)
    if g:
        next_fsm = g.group(1)
        step_fsm = g.group(2)
        all_arcs[cur_fsm, next_fsm] = step_fsm
        continue
    if ln != '},':
        print("?? %r" % (ln))

# Enable to calculate when P2P is not supported
if False:
    for c, n in list(all_arcs.keys()):
        s = all_arcs[c, n]
        while s != n and (s == "RUNNING_P2P" or s == "PRE_COPY_P2P"):
            s = all_arcs[s, n]
            all_arcs[c, n] = s
    for c, n in list(all_arcs.keys()):
        if c == "RUNNING_P2P" or c == "PRE_COPY_P2P":
            del all_arcs[c, n]

all_states = set(c for c, n in all_arcs.keys())
all_state_pairs = set(
    (c, n) for c, n in itertools.product(all_states, all_states) if c != n)
blocked_transitions = set(
    (c, n) for c, n in all_state_pairs if c != n and all_arcs[c, n] == "ERROR")
fsm_arcs = set((c, all_arcs[c, n]) for c, n in all_state_pairs
               if (c, n) not in blocked_transitions)

# Table is complete
for c, n in all_state_pairs:
    assert (c, n) in all_arcs

# Compute the combination paths through the FSM table
combination_paths = {}
for c, n in all_state_pairs - fsm_arcs - blocked_transitions:
    path = [c]
    while path[-1] != n:
        path.append(all_arcs[path[-1], n])
        assert "ERROR" not in path
    combination_paths[c, n] = path

# all_paths includes the FSM arcs too
all_paths = copy.copy(combination_paths)
for c, n in fsm_arcs - blocked_transitions:
    all_paths[c, n] = [c, n]

print("FSM arcs:", len(fsm_arcs))
for c, n in sorted(fsm_arcs):
    print("\t\t", c, "->", n)

print("Blocked transitions:", len(blocked_transitions))
for c, n in sorted(blocked_transitions, key=lambda x: ("ERROR" in x, x)):
    print("\t\t", c, "->", n)

print("Combination transitions:", len(combination_paths))
for path in sorted(combination_paths.values()):
    print("\t\t", " -> ".join(path))

print("Combination transitions (target sorted):")
for path in sorted(combination_paths.values(),
                   key=lambda x: list(reversed(x))):
    print("\t\t%60s" % (" -> ".join(path)))

print("Shortest path ambiguity:")
import networkx
fsm = networkx.DiGraph(list(fsm_arcs))
for c, n in sorted(all_state_pairs - blocked_transitions):
    paths = list(
        networkx.algorithms.shortest_paths.generic.all_shortest_paths(
            fsm, c, n))
    if len(paths) != 1:
        print(" ", c, "->", n)
        for path in sorted(paths):
            print(
                "    ", " -> ".join(path),
                "[selected]" if path == combination_paths.get((c, n)) else "")
    else:
        assert paths[0] == all_paths[c, n]

print("Error unwind study:")
for path in sorted(combination_paths.values()):
    print(" ", " -> ".join(path))
    for err in path[1:-1]:
        err_path = all_paths.get((err, path[0]))
        if err_path is None:
            print("   ", err, "-> ERROR")
            continue
        print("   ", " <- ".join(reversed(err_path)))

# Check mlx5 is implementing the right arcs
mlx5 = subprocess.check_output(
    ['grep', 'cur ==', "drivers/vfio/pci/mlx5/main.c"])
mlx5 = mlx5.decode()
mlx5_arcs = set()
for ln in mlx5.splitlines():
    ln = ln.strip()
    g = re.search(
        r"cur == VFIO_DEVICE_STATE_(.*) && new == VFIO_DEVICE_STATE_(.*?)\)",
        ln)
    if not g:
        print("??", repr(ln))
        continue
    mlx5_arcs.add((g.group(1), g.group(2)))
print("mlx5 extra arcs:", mlx5_arcs - fsm_arcs)
print("mlx5 missing arcs:", fsm_arcs - mlx5_arcs)
