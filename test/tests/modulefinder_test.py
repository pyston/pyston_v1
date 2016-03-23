# Make sure we can at least support people who want to register themselves with modulefinder,
# even if we don't actually support using modulefinder to find modules.

import modulefinder
modulefinder.AddPackagePath("foo", "bar")
