#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${project_root}/build-windows"
dist_dir="${project_root}/dist/HDRMerge-Windows-x64"
deps_dir="${RUNNER_TEMP:-${project_root}/.build-deps}/hdrmerge"
alglib_zip="${deps_dir}/alglib-3.15.0.cpp.gpl.zip"
alglib_root="${deps_dir}/alglib/cpp"

rm -rf "${build_dir}" "${project_root}/dist" "${deps_dir}/alglib"
mkdir -p "${build_dir}" "${dist_dir}" "${deps_dir}"

if [[ ! -f "${alglib_zip}" ]]; then
    curl --fail --location --retry 3 \
        https://www.alglib.net/translator/re/alglib-3.15.0.cpp.gpl.zip \
        --output "${alglib_zip}"
fi

unzip -q "${alglib_zip}" -d "${deps_dir}/alglib"

cmake -S "${project_root}" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DALGLIB_ROOT="${alglib_root}"
cmake --build "${build_dir}" --parallel

cp "${build_dir}/hdrmerge.exe" "${dist_dir}/"
cp "${build_dir}/hdrmerge-nogui.exe" "${dist_dir}/"

# Copy Qt libraries and the Windows platform plugin.
windeployqt-qt5 --release --no-translations --compiler-runtime "${dist_dir}/hdrmerge.exe"

# windeployqt only handles Qt. Resolve the complete MinGW dependency tree for
# HDRMerge, LibRaw, Exiv2, Qt and the deployed plugins.
copy_runtime_dependencies() {
    local copied=0
    local binary dependency destination

    while IFS= read -r -d '' binary; do
        while IFS= read -r dependency; do
            [[ -f "${dependency}" ]] || continue
            destination="${dist_dir}/$(basename "${dependency}")"
            if [[ ! -f "${destination}" ]]; then
                cp "${dependency}" "${destination}"
                copied=1
            fi
        done < <(ldd "${binary}" 2>/dev/null | awk '/=>/ { print $3 }' | grep "^${MINGW_PREFIX}/" || true)
    done < <(find "${dist_dir}" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0)

    if (( copied )); then
        return 0
    fi
    return 1
}

while copy_runtime_dependencies; do
    :
done

cp "${project_root}/LICENSE" "${dist_dir}/LICENSE.txt"
cp "${project_root}/README.md" "${dist_dir}/README.md"

cat > "${dist_dir}/BUILD-INFO.txt" <<EOF
HDRMerge Windows x64
Source revision: ${GITHUB_SHA:-local build}
Toolchain: MSYS2 UCRT64 x64
ALGLIB: 3.15.0 GPL
EOF

# A console-mode invocation catches missing runtime DLLs before publication.
"${dist_dir}/hdrmerge-nogui.exe" --help >/dev/null

echo "Portable build created at ${dist_dir}"
