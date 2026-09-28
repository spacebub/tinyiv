# x64-windows without the debug half, which takes as long again to build and is never stepped into.
# https://learn.microsoft.com/en-us/vcpkg/users/triplets#vcpkg_build_type
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)
