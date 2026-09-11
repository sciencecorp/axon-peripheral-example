# Shared SDK conftest — exposes the resolved SDK asset root for tests that need
# to locate vendored bitstreams, encrypted IP, or other build artifacts.
from axon_peripheral_sdk.profiles.paths import resolve_install_path

SDK_ASSET_ROOT = resolve_install_path("build/assets")
