# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""Build and cache the FinalizedSchedule of a LoopModel kernel."""

from dataclasses import dataclass

from ...Common.GlobalParameters import globalParameters
from ...LoopModel.checks import validate_loopir
from ...LoopModel.decoder import decode_theta
from ...LoopModel.loopir import render as render_loopir
from ...LoopModel.render import render_geometry
from ...Lowering.gir.analyses import Gl2PrefetchRegions
from ...Lowering.gir.finalizer import finalize_loop_schedule
from ...Lowering.gir.stages.manual_schedule import manual_schedule, schedule_cache_tag

from .Theta import loopModelTheta, namingSolution


@dataclass(frozen=True)
class _GirCacheEntry:
    """One kernel's FinalizedSchedule.

    An entry is reusable when the kernel name and schedule tag match and it
    was built with at least the verification and dependency detail requested.
    """

    key: object
    schedule: object
    verified: bool
    include_dependencies: bool
    schedule_tag: str

    def serves(self, key, verify_ir, include_dependencies, schedule_tag):
        return (self.key == key
                and (self.verified or not verify_ir)
                and (self.include_dependencies or not include_dependencies)
                and self.schedule_tag == schedule_tag)


def _valuAllocation(writer):
    """Each operand's `ValuAlloc` as vgprAllocationImplClassic sized it.

    Empty before allocation has run, which leaves the dump on theta's footprint.
    """
    return dict(getattr(writer.states, "loopModelValuAlloc", {}) or {})


def loopModelFinalizedSchedule(writer, kernel):
    """Build and cache the FinalizedSchedule for one kernel."""
    key = getattr(writer.states, "kernelName", None) or id(kernel)
    true_sia0 = int(kernel.get("ScheduleIterAlg", 0) or 0) == 0 \
        and int(kernel.get("_StinkyTofuOptLevel", 0) or 0) == 0
    dump_schedule = globalParameters.get("GIRScheduleDumpPath", "") if true_sia0 else ""
    load_schedule = globalParameters.get("GIRScheduleLoadPath", "") if true_sia0 else ""
    schedule_tag = schedule_cache_tag(str(key), dump_schedule, load_schedule)
    # Match the default this key carries in GlobalParameters.
    verify_ir = bool(globalParameters.get("LoopModelIRVerify", False))
    output_ir = bool(globalParameters.get("OutputLoopIR", False))
    include_dependencies = verify_ir or output_ir
    cache = getattr(writer, "_loopModelGirCache", None)
    if cache is not None and cache.serves(key, verify_ir, include_dependencies, schedule_tag):
        return cache.schedule

    theta, _S = loopModelTheta(writer, kernel)
    with namingSolution(writer, kernel):
        # The explicit checker below is the single verification point. Avoid
        # repeating its structural/profile checks in decode and lowering.
        decoded = decode_theta(theta, verify=False)
        if verify_ir:
            ir_violations = validate_loopir(
                decoded.program,
                decoded.profile,
            )
            if ir_violations:
                raise ValueError(
                    "UseLoopModel: LoopIR validation failed (%d violation(s)) — "
                    "repair the frontend, do not bypass the checker.\n  %s"
                    % (
                        len(ir_violations),
                        "\n  ".join(ir_violations[:8]),
                    )
                )
        manual = None
        if dump_schedule or load_schedule:
            manual = manual_schedule(
                key,
                dump_path=dump_schedule,
                load_path=load_schedule,
            )
        schedule = finalize_loop_schedule(
            decoded.program,
            decoded.profile,
            include_dependencies=include_dependencies,
            # PrefetchGL2 is a target-side GIR placement parameter, not LoopIR.
            params={Gl2PrefetchRegions.PARAM: kernel["PrefetchGL2"]},
            meta={"valu_alloc": {
                name: {
                    "per_buffer": int(alloc.perBuffer),
                    "total": int(alloc.total),
                }
                for name, alloc in (writer.states.loopModelValuAlloc or {}).items()
                if int(alloc.total) > 0
            }},
            manual=manual,
            verify=verify_ir,
            check=verify_ir,
            with_text=include_dependencies,
        )
        kinds = dict(schedule.dependency_kinds) if output_ir else {}
        if output_ir:
            # Cache before the semantic check so a failing kernel remains renderable when
            # OutputLoopIR is diagnosing that exact failure. The failed entry is explicitly
            # unverified, so no later verified request can reuse it as successful codegen.
            writer._loopModelMainloopCache = (key, decoded.program, kinds)
            writer._loopModelGirCache = _GirCacheEntry(
                key, schedule, False, include_dependencies, schedule_tag)
        if schedule.violations:
            raise ValueError(
                "UseLoopModel: the GIR plan fails its own semantic check (%d violation(s)) — "
                "a decoder/lowering defect to repair, do NOT gate it off.\n  %s"
                % (len(schedule.violations), "\n  ".join(schedule.violations[:8])))
    # Verification completed (or was not requested); publish the normal reusable cache state.
    writer._loopModelMainloopCache = (key, decoded.program, kinds)
    writer._loopModelGirCache = _GirCacheEntry(
        key, schedule, verify_ir, include_dependencies, schedule_tag)
    return schedule


def loopModelFinalizedScheduleCached(writer):
    """Return the cached schedule for the current named kernel, if any."""
    cache = getattr(writer, "_loopModelGirCache", None)
    if not cache:
        return None
    # Only the name is verifiable here; ids can be recycled by CPython.
    name = getattr(writer.states, "kernelName", None)
    return cache.schedule if (name and cache.key == name) else None


def loopModelIrTextCached(writer):
    """Render target geometry and the Program that produced the GIR."""
    name = getattr(writer.states, "kernelName", None)
    theta_cache = getattr(writer, "_loopModelThetaCache", None)
    mainloop_cache = getattr(writer, "_loopModelMainloopCache", None)
    if not (
        name
        and theta_cache
        and mainloop_cache
        and theta_cache[0] == name
        and mainloop_cache[0] == name
    ):
        return "# LoopModel IR unavailable: this kernel has no cached target-aware theta/IR\n"
    try:
        theta, _depths = theta_cache[1]
        program = mainloop_cache[1]
        if len(mainloop_cache) > 2:
            kinds = mainloop_cache[2]
        else:
            kinds = {}
        summary = ", ".join(
            f"{name}={count}"
            for name, count in sorted(kinds.items())
        ) or "none"
        return (
            render_geometry(theta, _valuAllocation(writer))
            + f"\n  derived_dependences: {summary}"
            + "\n\n"
            + render_loopir(program)
            + "\n"
        )
    except Exception as exc:
        return f"# LoopModel IR unavailable (cached render error): {exc}\n"


def loopModelGirTextCached(writer):
    """The `.gir` dump of the cached schedule."""
    schedule = loopModelFinalizedScheduleCached(writer)
    if schedule is None or schedule.gir_text is None:
        return "# GIR unavailable: this kernel has no GIR program\n"
    return schedule.gir_text


def attachFrameContract(writer, stModule, kernel):
    """Attach this kernel's frame contract to the StinkyTofu module."""
    from ...Lowering.gir_to_rocisa import FrameContractBuilder

    FrameContractBuilder(loopModelFinalizedSchedule(writer, kernel)).install(stModule)
