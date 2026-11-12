"""Project the authored call skip policy without probing optional media packages."""
import re

from selector_support import ROOT


def call_skip_policy():
    source = (ROOT / "gnome/groundhog/cmake/GroundhogCalls.cmake").read_text()
    functions = re.findall(
        r"(?ms)^function\(groundhog_call_test_skip_policy\)\n.*?^endfunction\(\)", source)
    if len(functions) != 1:
        raise ValueError("expected one authored Groundhog call skip-policy function")
    return functions[0] + "\n"
