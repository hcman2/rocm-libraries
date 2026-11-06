# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""Typed kinds, the stage table and the one published schedule."""

from tensilelite.Lowering.gir.emit_plan import EmitAction, plan_program
from tensilelite.Lowering.gir.frame_contract import build_contract
from tensilelite.Lowering.gir.kinds import EmitKind, MarkKind, MoveOp
from tensilelite.Lowering.gir.nodes import Mark, Move, Tile, Ref
from tensilelite.Lowering.gir import actions
from tensilelite.Lowering.gir.registry import ACTION_SPECS, MARK_SPECS
from tensilelite.Lowering.gir.render import inst_line
from tensilelite.Lowering.gir.schedule import finalize
from tensilelite.LoopModel.decoder import decode_theta
from tensilelite.Lowering.loopir_to_gir import lower_to_gir

from test_gir import build_gir
from test_loopmodel import BF16_NT_KMN, theta


def test_kinds_render_as_the_same_strings():
    tile = Tile("A", "shared")
    move = Move((Ref(tile),), (Ref(Tile("A", "register")),), op="ds_load")
    assert move.op is MoveOp.DS_LOAD
    assert inst_line(move).startswith("ds_load")

    mark = Mark("swap", {"hop": "ds_load", "gen_from": 0, "gen_to": 1,
                         "operand": "A", "steps": 1})
    assert mark.kind is MarkKind.SWAP
    assert not isinstance(mark.at, dict)
    assert mark.at["hop"] == "ds_load"
    assert f"{MoveOp.DS_LOAD:11s}" == "ds_load    "
    assert hash(MoveOp.DS_LOAD) == hash("ds_load")
    assert {"ds_load": 1}[MoveOp.DS_LOAD] == 1
    try:
        mark.at["hop"] = "tensor_load"
    except TypeError:
        pass
    else:
        raise AssertionError("a payload accepted an item assignment")
    nested = Mark("region_increment", {
        "unit": ("A",), "steps": 1, "from_region": 0, "to_region": 1,
        "axis_steps": {"k": 1}})
    try:
        nested.at["axis_steps"]["k"] = 2
    except TypeError:
        pass
    else:
        raise AssertionError("a nested payload dict accepted an item assignment")
    assert inst_line(mark) == (
        "mark swap {'hop': 'ds_load', 'gen_from': 0, 'gen_to': 1, "
        "'operand': 'A', 'steps': 1}")

    action = EmitAction("ds_load", {"tc": "A", "tile": 0, "k": 0, "reg_buf": 0})
    assert action.kind is EmitKind.DS_LOAD
    assert action.at["tc"] == "A"
    assert action.kind == "ds_load"


def test_registry_covers_every_mark_and_emitted_kind():
    from tensilelite.Lowering.gir_to_rocisa import GirToRocisa

    assert set(MARK_SPECS) == set(MarkKind)
    assert set(ACTION_SPECS) == set(EmitKind)
    for spec in MARK_SPECS.values():
        assert spec.emits is None or spec.emits in ACTION_SPECS
    for kind, spec in ACTION_SPECS.items():
        assert issubclass(spec.action, actions.EmitAction)
        assert spec.method is None or callable(getattr(GirToRocisa, spec.method)), kind
    silent = {kind for kind, spec in ACTION_SPECS.items() if spec.method is None}
    assert silent == {EmitKind.STORE}


def test_actions_are_typed_by_kind():
    read = EmitAction("ds_load", {"tc": "A"})
    assert type(read) is actions.ReadAction
    assert type(EmitAction(kind="global_load", at={})) is actions.ReadAction
    assert type(EmitAction(EmitKind.WMMA, {})) is actions.WmmaAction
    assert type(EmitAction("swap", {})) is actions.SwapAction
    assert type(EmitAction("tensor_load", {})) is actions.CopyAction
    from dataclasses import replace
    assert type(replace(read, action_id=3)) is actions.ReadAction
    assert read.spec is ACTION_SPECS[EmitKind.DS_LOAD]
    try:
        actions.WmmaAction("ds_load", {})
    except TypeError:
        pass
    else:
        raise AssertionError("a wmma action accepted a read kind")
    try:
        EmitAction("nonsense", {})
    except KeyError:
        pass
    else:
        raise AssertionError("an unknown action kind was accepted")


def test_every_action_declares_its_storage():
    for kind, spec in ACTION_SPECS.items():
        assert isinstance(spec.touches_shared, bool), kind
    shared = {kind for kind, spec in ACTION_SPECS.items() if spec.touches_shared}
    assert shared == {EmitKind.DS_LOAD, EmitKind.GLOBAL_LOAD, EmitKind.TENSOR_LOAD,
                      EmitKind.STORE}


def test_published_schedule_matches_the_separate_derivations():
    program = build_gir(theta(dict(BF16_NT_KMN)))
    separate_plans = plan_program(program)
    separate_contract = build_contract(program)
    schedule = finalize(program)

    assert schedule.plans.keys() == separate_plans.keys()
    for label, actions in schedule.plans.items():
        other = separate_plans[label]
        assert [item.action_id for item in actions] == [item.action_id for item in other]
        assert [item.kind for item in actions] == [item.kind for item in other]
        assert [dict(item.at) for item in actions] == [dict(item.at) for item in other]
    assert schedule.action_ids == tuple(
        item.action_id for actions in separate_plans.values() for item in actions)
    flat = [access for action_id in schedule.action_ids
            for access in schedule.accesses_of(action_id)]
    assert flat == list(separate_contract.accesses)
    assert list(schedule.contract.accesses) == flat
    for actions in schedule.plans.values():
        for action in actions:
            assert action.source is None
            assert schedule.lines[action.action_id]
    assert schedule.contract.edges == separate_contract.edges
    assert schedule.contract.requires == separate_contract.requires
    assert schedule.version == program.version
    assert not hasattr(schedule, "program")
    assert isinstance(schedule.plans["prologue"], tuple)
    assert isinstance(schedule.contract.accesses, tuple)
    assert isinstance(schedule.contract.edges, tuple)
    try:
        schedule.plans["prologue"] = ()
    except TypeError:
        pass
    else:
        raise AssertionError("published plans accepted a replacement")
    try:
        schedule.contract.generations[0] = None
    except TypeError:
        pass
    else:
        raise AssertionError("published generations accepted an assignment")


def _schedule_facts(schedule):
    contract = schedule.contract
    return (
        schedule.gir_text,
        [(phase, [(a.action_id, str(a.kind), dict(a.at)) for a in actions])
         for phase, actions in schedule.plans.items()],
        dict(schedule.lines),
        sorted(contract.generations.values(), key=lambda g: g.id),
        sorted(contract.actions.values(), key=lambda a: a.id),
        contract.accesses, contract.edges, contract.requires,
    )


DIFFERENTIAL_CASES = (
    dict(BF16_NT_KMN),
    dict(BF16_NT_KMN, LoopOrder="NMK"),
    dict(BF16_NT_KMN, LoopOrder="MKN", PrefetchGlobalRead=1),
    dict(BF16_NT_KMN, PrefetchGlobalRead=3),
    dict(BF16_NT_KMN, PrefetchLocalRead=2),
)


def test_direct_finalizer_matches_lowering_plus_build():
    """finalize_loop_schedule(LoopIR) against lower_to_gir + build_program + finalize."""
    from tensilelite.Lowering.gir.finalizer import build_program, finalize_loop_schedule

    for params in DIFFERENTIAL_CASES:
        old_decoded = decode_theta(theta(dict(params)))
        old = lower_to_gir(old_decoded.program, old_decoded.profile)
        build_program(old)
        expected = finalize(old, with_text=True)

        new_decoded = decode_theta(theta(dict(params)))
        actual = finalize_loop_schedule(new_decoded.program, new_decoded.profile,
                                        with_text=True)
        assert _schedule_facts(actual) == _schedule_facts(expected), params


def test_build_program_freezes_the_program():
    import pytest
    from tensilelite.Lowering.gir.finalizer import build_program

    program = build_gir(theta(dict(BF16_NT_KMN)))
    assert program.frozen
    with pytest.raises(RuntimeError):
        program.bump()
    with pytest.raises(RuntimeError):
        build_program(program)
    block = next(iter(program.blocks.values()))
    with pytest.raises(AttributeError):
        block.body = ()
    with pytest.raises(AttributeError):
        block.body[0].deps = ()
    with pytest.raises(TypeError):
        program.blocks["x"] = block
    with pytest.raises(TypeError):
        program.meta["x"] = 1
    assert isinstance(block.body, tuple)


def test_finalize_refuses_an_unfinished_program():
    import pytest

    decoded = decode_theta(theta(dict(BF16_NT_KMN)))
    with pytest.raises(RuntimeError):
        finalize(lower_to_gir(decoded.program, decoded.profile))


def test_access_declarations_match_what_actions_touch():
    """Every action with storage touches comes from a kind declared to have them."""
    program = build_gir(theta(dict(BF16_NT_KMN)))
    schedule = finalize(program)
    for actions in schedule.plans.values():
        for action in actions:
            if schedule.accesses_of(action.action_id):
                assert action.spec.touches_shared, action.kind
    touched = {action.kind for actions in schedule.plans.values() for action in actions
               if schedule.accesses_of(action.action_id)}
    assert {EmitKind.DS_LOAD, EmitKind.TENSOR_LOAD} <= touched


def test_manual_schedule_round_trips_and_rejects_a_foreign_format(tmp_path):
    import pytest
    import yaml
    from tensilelite.Lowering.gir.finalizer import finalize_loop_schedule
    from tensilelite.Lowering.gir.stages.manual_schedule import (
        FORMAT, manual_schedule, schedule_file)

    def build(**paths):
        decoded = decode_theta(theta(dict(BF16_NT_KMN)))
        return finalize_loop_schedule(decoded.program, decoded.profile, with_text=True,
                                      manual=manual_schedule("K", **paths))

    dumped = build(dump_path=tmp_path)
    path = schedule_file(tmp_path, "K")
    document = yaml.safe_load(path.read_text())
    assert document["format"] == FORMAT
    assert build(load_path=tmp_path).gir_text == dumped.gir_text

    path.write_text(path.read_text().replace(FORMAT, "tensilelite-gir-schedule-v0"))
    with pytest.raises(RuntimeError, match="expected %r" % FORMAT):
        build(load_path=tmp_path)


def test_manual_schedule_round_trips_and_rejects_a_foreign_format(tmp_path):
    import pytest
    import yaml
    from tensilelite.Lowering.gir.finalizer import finalize_loop_schedule
    from tensilelite.Lowering.gir.stages.manual_schedule import (
        FORMAT, manual_schedule, schedule_file)

    def build(**paths):
        decoded = decode_theta(theta(dict(BF16_NT_KMN)))
        return finalize_loop_schedule(decoded.program, decoded.profile, with_text=True,
                                      manual=manual_schedule("K", **paths))

    dumped = build(dump_path=tmp_path)
    path = schedule_file(tmp_path, "K")
    document = yaml.safe_load(path.read_text())
    assert document["format"] == FORMAT
    assert build(load_path=tmp_path).gir_text == dumped.gir_text

    path.write_text(path.read_text().replace(FORMAT, "tensilelite-gir-schedule-v0"))
    with pytest.raises(RuntimeError, match="expected %r" % FORMAT):
        build(load_path=tmp_path)
