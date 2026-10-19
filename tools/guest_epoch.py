"""Explicit reproducibility epoch for Apple guest compilation only."""
import re

FIELD = "guest_source_date_epoch"
ENVIRONMENT = "VISR_GUEST_SOURCE_DATE_EPOCH"


def parse_guest_epoch(value: object) -> str:
    if (not isinstance(value, str) or not re.fullmatch(r"[1-9][0-9]{0,11}", value)
            or int(value) > 253402300799):
        raise ValueError("Guest source epoch must be canonical decimal seconds in 1..253402300799")
    return value


def guest_epoch_prefix(value: str | None) -> str:
    return "" if value is None else f"env SOURCE_DATE_EPOCH={parse_guest_epoch(value)} "
