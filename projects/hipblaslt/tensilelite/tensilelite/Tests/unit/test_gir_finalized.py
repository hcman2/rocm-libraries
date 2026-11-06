# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""Typed kinds, one published schedule, and stage parity with the flat pipeline."""

from tensilelite.Lowering.gir.emit_plan import EmitAction, plan_program
from tensilelite.Lowering.gir.frame_contract import build_contract
from tensilelite.Lowering.gir.kinds import EmitKind, MarkKind, MoveOp
from tensilelite.Lowering.gir.nodes import Mark, Move, Tile, Ref
from tensilelite.Lowering.gir.registry import EMIT_METHOD, MARK_EMIT, MARK_SCHEMA
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
    assert isinstance(mark.at, dict)
    assert inst_line(mark) == (
        "mark swap {'hop': 'ds_load', 'gen_from': 0, 'gen_to': 1, "
        "'operand': 'A', 'steps': 1}")

    action = EmitAction("ds_load", {"tc": "A", "tile": 0, "k": 0, "reg_buf": 0})
    assert action.kind is EmitKind.DS_LOAD
    assert action.at["tc"] == "A"
    assert action.kind == "ds_load"


def test_registry_covers_every_mark_and_emitted_kind():
    assert set(MARK_SCHEMA) == set(MarkKind)
    assert set(MARK_EMIT) == set(MarkKind)
    emitted = {kind for kind in MARK_EMIT.values() if kind is not None}
    assert emitted <= set(EMIT_METHOD)
    for kind in (EmitKind.DS_LOAD, EmitKind.GLOBAL_LOAD, EmitKind.TENSOR_LOAD,
                 EmitKind.WMMA, EmitKind.SWAP, EmitKind.GR_INC, EmitKind.REGION_INC,
                 EmitKind.DESC_ENABLE, EmitKind.GL2_PREFETCH):
        assert EMIT_METHOD[kind]


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
    assert list(schedule.accesses) == list(separate_contract.accesses)
    assert schedule.contract.edges == separate_contract.edges
    assert schedule.contract.requires == separate_contract.requires
    assert schedule.version == program.version


def test_each_stage_matches_the_flat_pipeline():
    from tensilelite.Lowering.gir.finalizer import compare_stage_path

    def make():
        decoded = decode_theta(theta(dict(BF16_NT_KMN)))
        return lower_to_gir(decoded.program, decoded.profile)

    assert compare_stage_path(make)
