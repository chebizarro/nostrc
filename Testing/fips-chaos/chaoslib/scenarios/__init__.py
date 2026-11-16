"""Scenario modules: each exposes VARIANTS and run(ctx, variant) -> dict of measurements."""
from . import flap, mtu, nat, netem  # noqa: F401

ALL = {'flap': flap, 'mtu': mtu, 'nat': nat, 'netem': netem}
