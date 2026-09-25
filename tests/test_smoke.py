from importlib.metadata import version

import fb200


def test_version_matches_metadata():
    assert fb200.__version__ == version("fb200-tools")
