# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""Typed emit actions.

``EmitAction(kind, at)`` returns the action class registered for ``kind``.
Every action keeps ``kind`` and ``at``, so code that reads those fields does
not care which class it got.
"""

from __future__ import annotations

from dataclasses import dataclass

from .kinds import coerce_emit


@dataclass(frozen=True)
class EmitAction:
    """One thing L3 must emit: its `kind`, and the `at` payload that kind needs."""
    kind: str
    at: dict
    # Dense, deterministic identity in Program block/action order. ``source`` is the GIR node
    # that produced this action and is intentionally not serialized by ordinary emitters.
    action_id: int = -1
    source: object = None

    def __new__(cls, *args, **kwargs):
        if cls is EmitAction:
            kind = args[0] if args else kwargs.get("kind")
            cls = _registry.action_spec(coerce_emit(kind)).action
        return object.__new__(cls)

    def __post_init__(self):
        kind = coerce_emit(self.kind)
        spec = _registry.action_spec(kind)
        if not isinstance(self, spec.action):
            raise TypeError("%s cannot carry kind %r" % (type(self).__name__, kind))
        object.__setattr__(self, "kind", kind)
        object.__setattr__(self, "at", _registry.payload_for(kind, self.at))

    @property
    def spec(self):
        return _registry.action_spec(self.kind)


class ReadAction(EmitAction):
    """shared (or global, DirectToVgpr) -> register."""


class CopyAction(EmitAction):
    """global -> shared."""


class StoreAction(EmitAction):
    """register -> global. Planned for completeness; emits nothing."""


class WmmaAction(EmitAction):
    """One matrix instruction."""


class SwapAction(EmitAction):
    """A buffer-pointer swap on a read or copy hop."""


class GrIncAction(EmitAction):
    """One summation-chunk step of a global address."""


class RegionIncAction(EmitAction):
    """A TDM descriptor walk across storage regions."""


class DescEnableAction(EmitAction):
    """Null or restore one member of a fused descriptor."""


class Gl2PrefetchAction(EmitAction):
    """The L2 warm-up pair."""


from . import registry as _registry  # noqa: E402  (registry imports the classes above)
