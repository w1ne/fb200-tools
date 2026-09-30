# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

from importlib.metadata import version

import fb200


def test_version_matches_metadata():
    assert fb200.__version__ == version("fb200-tools")
