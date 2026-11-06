# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""Who may depend on what around GIR.

GIR core states dataflow and placement. The rocisa adapter is the only module
that reaches kernel-writer and hardware emitters. StinkyTofu alone decides
barriers, waits and fences, so GIR hazard analyses may only verify or order
actions.
"""

import ast
import dataclasses
import pathlib

import tensilelite.Lowering.gir as gir_package
from tensilelite.Lowering.gir.frame_contract import Contract

GIR_ROOT = pathlib.Path(gir_package.__file__).resolve().parent
LOWERING_ROOT = GIR_ROOT.parent
PACKAGE_ROOT = LOWERING_ROOT.parent

#: Modules GIR core must not import, matched against the absolute module path.
BACKEND = ("rocisa", "stinkytofu", "KernelWriter", "KernelWriterAssembly",
           "Components.TDMFuse", "Lowering.leaves", "Lowering.gir_to_rocisa")


def _imports(path):
    """Absolute dotted names imported by one file (relative imports resolved)."""
    rel = path.relative_to(PACKAGE_ROOT.parent).with_suffix("")
    package = list(rel.parts[:-1])
    out = []
    for node in ast.walk(ast.parse(path.read_text())):
        if isinstance(node, ast.Import):
            out += [alias.name for alias in node.names]
        elif isinstance(node, ast.ImportFrom):
            base = package[:len(package) - node.level + 1] if node.level else []
            module = ".".join(base + ([node.module] if node.module else []))
            out.append(module)
            out += [module + "." + alias.name for alias in node.names]
    return out


def _names(name, wanted):
    """Does the dotted ``name`` contain ``wanted`` as whole components?"""
    parts, want = name.split("."), wanted.split(".")
    return any(parts[i:i + len(want)] == want for i in range(len(parts)))


def _hits(names, banned):
    return sorted({name for name in names for ban in banned if _names(name, ban)})


def test_gir_core_imports_no_backend():
    offenders = {}
    for path in sorted(GIR_ROOT.rglob("*.py")):
        hits = _hits(_imports(path), BACKEND)
        if hits:
            offenders[str(path.relative_to(GIR_ROOT))] = hits
    assert offenders == {}


def _importers(module, roots):
    return {str(path.relative_to(PACKAGE_ROOT))
            for root in roots for path in root.rglob("*.py")
            if any(_names(name, module) for name in _imports(path))}


def test_hazard_analyses_only_verify_or_order_actions():
    roots = (LOWERING_ROOT, PACKAGE_ROOT / "Components")
    frame = _importers("analyses.frame_hazards", roots)
    reg = _importers("analyses.reg_hazards", roots)
    assert frame == {"Lowering/gir/verify.py", "Lowering/gir/passes/hoist_copies.py",
                     "Lowering/gir/analyses/reg_hazards.py"}
    assert reg == {"Lowering/gir/passes/hoist_copies.py"}


def test_frame_contract_carries_state_not_synchronization_policy():
    assert {f.name for f in dataclasses.fields(Contract)} == {
        "generations", "actions", "accesses", "edges", "requires"}
