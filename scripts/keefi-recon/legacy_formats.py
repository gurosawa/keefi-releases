"""Read the previous export envelope without changing its calculation inputs."""

LEGACY_BUNDLE = "finix-valuation-bundle/1"
CURRENT_BUNDLE = "keefi-valuation-bundle/1"
LEGACY_RESULT = "finix_result"
CURRENT_RESULT = "keefi_result"


def normalize_bundle(value):
    if not isinstance(value, dict):
        raise ValueError("valuation bundle must be an object")
    if value.get("format") == LEGACY_BUNDLE:
        if CURRENT_RESULT in value:
            raise ValueError("mixed export identities")
        current = dict(value)
        current["format"] = CURRENT_BUNDLE
        if LEGACY_RESULT in current:
            current[CURRENT_RESULT] = current.pop(LEGACY_RESULT)
        return current
    if value.get("format") == CURRENT_BUNDLE and LEGACY_RESULT in value:
        raise ValueError("mixed export identities")
    return value
