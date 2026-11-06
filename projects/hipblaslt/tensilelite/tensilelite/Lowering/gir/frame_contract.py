# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""Build the GIR frame facts handed to StinkyTofu."""

from __future__ import annotations

from dataclasses import dataclass
from types import MappingProxyType

from .analysis import AnalysisManager
from .analyses.frame_map import FrameMap
from .analyses.cfg import reachable
from .analyses.trip_domains import TripDomains
from .emit_plan import plan_program
from .nodes import Move, successor_labels
from .storage import SharedAccess, steady_frame_basis, storage_accesses  # noqa: F401


# --- the facts ---------------------------------------------------------------


@dataclass(frozen=True)
class Generation:
    """A rotating buffer: how deep it is, where it enters, how far each trip advances it."""
    id: int
    ring: int
    entry: int = 0
    advance: int = 0


@dataclass(frozen=True)
class Action:
    """One schedulable unit, and the action whose position its block is pinned to."""
    id: int
    kind: str
    anchor: int


@dataclass(frozen=True)
class Edge:
    """A generation's phase arriving at `dst` from `src`.

    `relative` distinguishes the two ways GIR states it: a `transfer` forwards the predecessor's
    phase shifted by `delta`, an `incoming` pins it to an absolute `value`.
    """
    dst: int
    src: int
    gen: int
    value: int
    relative: bool


@dataclass(frozen=True)
class Requires:
    """An edge is takeable only when `gen` carries one of `values`.

    The mirror of `Edge`: `incoming` ASSIGNS a phase on an edge, `requires` CONSTRAINS one. A
    guard generation records which arm of a correlated branch ran, so a consuming edge can refuse
    the arms that cannot reach it.  Absence of a record constrains nothing.
    """
    dst:    int
    src:    int
    gen:    int
    values: tuple


@dataclass(frozen=True)
class Contract:
    """Frame facts that can no longer be edited after publication.

    ``generations`` and ``actions`` are mapping proxies. ``accesses``,
    ``edges`` and ``requires`` are tuples. Their members are frozen too.
    """

    generations: object
    actions: object
    accesses: tuple
    edges: tuple
    requires: tuple


# --- deriving the facts from a program ---------------------------------------


def _generations(prog) -> dict:
    """Every rotating buffer the program names, with its ring, entry phase and per-trip advance."""
    facts = {}

    def ensure(gen, ring=None):
        if gen is None:
            return None
        depth = max(1, int(ring if ring is not None else gen.ring))
        fact = facts.setdefault(int(gen.id), {"ring": depth, "entry": None, "advance": None})
        if fact["ring"] != depth:
            raise RuntimeError("frame contract: inconsistent ring for Gen %d" % gen.id)
        return fact

    for block in prog.blocks.values():
        for phi in block.phis:
            fact = ensure(phi.gen)
            if phi.entry_val is None:
                continue
            value = int(phi.entry_val) % fact["ring"]
            if fact["entry"] not in (None, value):
                raise RuntimeError("frame contract: inconsistent phi entry for Gen %d" % phi.gen.id)
            fact["entry"] = value
        for xfer in block.xfers:
            fact = ensure(xfer.gen, xfer.ring)
            value = int(xfer.adv) % fact["ring"]
            if fact["advance"] not in (None, value):
                raise RuntimeError("frame contract: inconsistent transfer for Gen %d" % xfer.gen.id)
            fact["advance"] = value
        for edge_xfers in (block.edge_xfers or {}).values():
            for xfer in edge_xfers:
                ensure(xfer.gen, xfer.ring)
        for inst in block.body:
            if isinstance(inst, Move):
                for ref in tuple(inst.srcs) + tuple(inst.dsts):
                    ensure(getattr(ref, "gen", None))

    return {gid: Generation(gid, fact["ring"], fact["entry"] or 0, fact["advance"] or 0)
            for gid, fact in facts.items()}


def _anchor_actions(prog, plans, label, first, seen=()):
    """The action ids that stand for `label` on an incoming (`first`) or outgoing edge.

    The FIRST action is a block's canonical identity: ST scheduling may reorder physical
    instructions, so an outgoing frame must never depend on whichever action is emitted last.
    An empty block forwards the question to its neighbours.
    """
    if label in seen:
        return ()
    actions = plans.get(label, ())
    if actions:
        return (actions[0].action_id,)
    neighbours = prog.blocks[label].succs if first else prog.blocks[label].preds
    out = set()
    for neighbour in neighbours:
        if neighbour in prog.blocks:
            out.update(_anchor_actions(prog, plans, neighbour, first, tuple(seen) + (label,)))
    return tuple(sorted(out))


def _forward_delta(frames, predecessor, destination, gen) -> int:
    """The affine phase transfer of one forwarding phi input.

    `None` in a GenPhi does not mean "no edge semantics": it forwards the value after GIR's edge
    transfer. Deriving that from FrameMap is what stops the serialized contract substituting ST's
    loop-edge convention for GIR's exact dataflow.
    """
    ring = max(1, int(gen.ring))
    deltas = {
        (target.of(gen.id) - source.of(gen.id)) % ring
        for source in frames.frames(predecessor)
        for label, target in frames.successors((predecessor, source))
        if label == destination
    }
    if len(deltas) != 1:
        raise RuntimeError(
            "frame contract: phi transfer %s->%s for Gen %d is not one affine delta: %r"
            % (predecessor, destination, gen.id, sorted(deltas)))
    return next(iter(deltas))


def _edges_of(prog, plans, frames, generations, basis):
    """Every phi input, as the phase one block's generation receives from another."""
    out = set()
    stated = set()
    for block in prog.blocks.values():
        destinations = _anchor_actions(prog, plans, block.label, True)
        for phi in block.phis:
            for pred, value in phi.incomings:
                sources = _anchor_actions(prog, plans, pred, False)
                if not sources or not destinations:
                    raise RuntimeError(
                        "frame contract: phi input %s->%s has no physical action anchor"
                        % (pred, block.label))
                relative = value is None
                ring = max(1, int(phi.gen.ring))
                phase = (
                    (_forward_delta(frames, pred, block.label, phi.gen)
                     + basis.get(block.label, 0) - basis.get(pred, 0)) % ring
                    if relative
                    else (int(value) + basis.get(block.label, 0)) % ring
                )
                for source in sources:
                    for destination in destinations:
                        out.add(Edge(int(destination), int(source), int(phi.gen.id),
                                     phase, relative))
                stated.add((pred, block.label, int(phi.gen.id)))

    # A multi-copy steady body is one logical loop trip split across several physical blocks.
    # Its non-header edges have no phi, but the frame still advances by one copy. State that
    # transition explicitly so the consumer does not have to recover it from per-access gdeltas.
    for source_label, source_block in prog.blocks.items():
        for destination_label in source_block.succs:
            if destination_label not in prog.blocks:
                continue
            delta = basis.get(destination_label, 0) - basis.get(source_label, 0)
            if not delta:
                continue
            sources = _anchor_actions(prog, plans, source_label, False)
            destinations = _anchor_actions(prog, plans, destination_label, True)
            if not sources or not destinations:
                continue
            for gen_id, generation in generations.items():
                if (source_label, destination_label, gen_id) in stated:
                    continue
                phase = delta % max(1, int(generation.ring))
                for source in sources:
                    for destination in destinations:
                        out.add(Edge(int(destination), int(source), int(gen_id), phase, True))
    return sorted(out, key=lambda e: (e.relative, e.dst, e.src, e.gen, e.value))


#: A guard generation is not a buffer -- it never rotates, and 0 means the arm is undecided.
GUARD_UNDECIDED = 0


def _guards_of(prog, plans, domains, first_free_gen):
    """Guard generations for the branches whose ARM CHOICE some later edge can refuse.

    A branch whose arms all reach every consumer carries no information, so it gets no generation;
    allocating one per correlated branch is what lets the passes that build them stay ignorant of
    each other -- they agree on a generation id, never on a predicate. Counted-loop edge domains
    let an earlier branch choice constrain later back and exit edges by the same general rule.
    """
    def arms_of(label):
        out = []
        for target in dict.fromkeys(successor_labels(prog.blocks[label])):
            if target in prog.blocks:
                out.append((target, domains.admits(label, target)))
        return out

    everywhere = [(lab, arms_of(lab)) for lab in prog.blocks]
    consuming = [(src, dst, domains.admits(src, dst))
                 for src, dst, in ((s, d) for s in prog.blocks for d in dict.fromkeys(
                     successor_labels(prog.blocks[s])) if d in prog.blocks)]

    generations, incomings, requires = {}, [], []
    prefetch = (prog.meta or {}).get("prefetch_guard") or {}
    for label, arms in everywhere:
        if len(arms) < 2 or all(d == domains.universe for _t, d in arms):
            continue
        # Only worth a generation if some edge admits one arm and refuses another -- and only
        # where the arm has already been taken, since an edge before the branch reads `undecided`
        # and would carry a constraint it can never fail.
        downstream = reachable(prog)
        after = {t for t, _d in arms} | {r for t, _d in arms for r in downstream.get(t, ())}
        refusers = [(src, dst, edge) for src, dst, edge in consuming
                    if src in after
                    and any(not (d & edge) for _t, d in arms) and any(d & edge for _t, d in arms)
                    and (src, dst) not in {(label, t) for t, _d in arms}]
        if not refusers:
            continue
        gen_id = first_free_gen + len(generations)
        generations[label] = (gen_id, arms)
        for index, (target, _domain) in enumerate(arms, start=1):
            for source in _anchor_actions(prog, plans, label, False):
                for destination in _anchor_actions(prog, plans, target, True):
                    incomings.append(Edge(int(destination), int(source), gen_id, index, False))
                    # The source is undecided before its first branch. If this edge is revisited
                    # after an arm value has propagated around the CFG, only that same arm may
                    # take it. This is a general guard rule; scaffold labels play no role here.
                    requires.append(Requires(
                        int(destination), int(source), gen_id,
                        (GUARD_UNDECIDED, index)))
        # The short arm skips the peeled copy and reaches `prologue_join`, but it does not enter
        # the modeled steady software pipeline. Constrain that join explicitly: otherwise its
        # one-time rank-0 RAW is merged into the recurring loop-header requirement.
        targets = {target for target, _domain in arms}
        if "steady1" in prog.blocks \
                and targets == {prefetch.get("block"), prefetch.get("skip")}:
            full = next(index for index, (target, _domain) in enumerate(arms, start=1)
                        if target == prefetch.get("block"))
            join = prefetch.get("join")
            if join in prog.blocks:
                for successor in prog.blocks[join].succs:
                    if successor not in prog.blocks:
                        continue
                    for source in _anchor_actions(prog, plans, join, False):
                        for destination in _anchor_actions(prog, plans, successor, True):
                            requires.append(
                                Requires(int(destination), int(source), gen_id, (full,)))
        for src, dst, edge in refusers:
            allowed = tuple(sorted(
                [GUARD_UNDECIDED] + [i for i, (_t, d) in enumerate(arms, start=1) if d & edge]))
            for source in _anchor_actions(prog, plans, src, False):
                for destination in _anchor_actions(prog, plans, dst, True):
                    requires.append(Requires(int(destination), int(source), gen_id, allowed))
    guard_gens = {gen_id: Generation(gen_id, len(arms) + 1, GUARD_UNDECIDED, 0)
                  for gen_id, arms in generations.values()}
    return guard_gens, incomings, requires


def _steady_copy_guard(prog, plans, gen_id):
    """One-way control state for an unrolled steady-copy chain."""
    if "steady" not in prog.blocks:
        return {}, [], []
    chain = ["steady"]
    while "steady%d" % len(chain) in prog.blocks:
        chain.append("steady%d" % len(chain))
    if len(chain) <= 1:
        return {}, [], []

    # State 0 is the one-time entry into copy 0; state 1 is recurring copy 0. Keeping them
    # distinct prevents the finite frame graph from aliasing the prologue with a later ring wrap.
    generation = Generation(gen_id, len(chain) + 1, 0, 0)
    incomings, requires = [], []

    def edge(source_label, destination_label, source_values, destination_value):
        if destination_label not in prog.blocks[source_label].succs:
            return
        sources = _anchor_actions(prog, plans, source_label, False)
        destinations = _anchor_actions(prog, plans, destination_label, True)
        for source in sources:
            for destination in destinations:
                incomings.append(
                    Edge(int(destination), int(source), gen_id, int(destination_value), False))
                requires.append(
                    Requires(int(destination), int(source), gen_id,
                             tuple(int(value) for value in source_values)))

    # Entry is copy 0; every intra-chain edge is a one-way jump to the next copy.
    first = chain[0]
    for predecessor in prog.blocks[first].preds:
        if predecessor not in chain:
            edge(predecessor, first, (0,), 0)
    for index, source in enumerate(chain[:-1]):
        destination = chain[index + 1]
        source_values = (0, 1) if index == 0 else (index + 1,)
        edge(source, destination, source_values, index + 2)
    edge(chain[-1], first, (len(chain),), 1)
    return {gen_id: generation}, incomings, requires


def build_contract(prog, plans=None, accesses=None) -> Contract:
    """The frame facts of `prog`, with no GIR CFG topology in them.

    ``plans`` and ``accesses`` are the program's emit plan and its storage
    projection when the caller already built them. The contract never derives
    an access itself, so it and the instruction stamps share one source.
    """
    analyses = AnalysisManager()
    plans = plan_program(prog) if plans is None else plans
    accesses = storage_accesses(prog, plans, analyses) if accesses is None else accesses
    frames = analyses.get(FrameMap(), prog)

    anchors = {action.action_id: block_actions[0].action_id
               for block_actions in plans.values() if block_actions
               for action in block_actions}

    generations = _generations(prog)
    actions = {}
    for block_actions in plans.values():
        for action in block_actions:
            actions[action.action_id] = Action(
                action.action_id, action.kind, anchors[action.action_id])
    flat_accesses = tuple(access for action_id in actions for access in accesses[action_id])
    basis = steady_frame_basis(prog)
    edges = list(_edges_of(prog, plans, frames, generations, basis))
    domains = analyses.get(TripDomains(), prog)
    first_free = max(generations, default=-1) + 1
    guard_gens, guard_incomings, guard_requires = _guards_of(prog, plans, domains, first_free)
    generations.update(guard_gens)
    edges.extend(guard_incomings)
    requires = list(guard_requires)
    copy_gen = max(generations, default=-1) + 1
    copy_gens, copy_incomings, copy_requires = _steady_copy_guard(
        prog, plans, copy_gen)
    generations.update(copy_gens)
    edges.extend(copy_incomings)
    requires.extend(copy_requires)
    return Contract(
        generations=MappingProxyType(generations),
        actions=MappingProxyType(actions),
        accesses=flat_accesses,
        edges=tuple(edges),
        requires=tuple(requires),
    )


def install_frame_contract(prog, st_module):
    """Hand the contract to StinkyTofu as a STRUCT -- the one call the kernel writer makes.

    Only what belongs to no instruction travels: the generation table, the phi edges and the guard
    constraints.  Actions and the storage they touch ride on the instructions, and nothing is
    encoded, so there is no text format for a writer and a reader to disagree about.
    """
    from rocisa import GirFrameContract
    from .schedule import published_schedule

    contract = published_schedule(prog).contract
    out = GirFrameContract()
    for generation in sorted(contract.generations.values(), key=lambda g: g.id):
        out.addGeneration(id=generation.id, ring=generation.ring, entry=generation.entry)
    for edge in contract.edges:
        out.addIncoming(dst=edge.dst, src=edge.src, gen=edge.gen, value=edge.value,
                        relative=edge.relative)
    for need in contract.requires:
        out.addRequires(dst=need.dst, src=need.src, gen=need.gen, values=list(need.values))
    st_module.setGirFrameContract(out)
    return contract
