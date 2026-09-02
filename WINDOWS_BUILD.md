# Windows x64 build with GitHub Actions

The workflow in `.github/workflows/windows.yml` creates a portable Windows x64
artifact using MSYS2, MinGW-w64 and Qt 5. It can be run on a free GitHub account
when the repository is public.

## Run the build

1. Fork `jcelaya/hdrmerge` on GitHub.
2. Apply the files and changes from this working tree to the fork.
3. Open the fork's **Actions** tab and enable workflows if GitHub asks.
4. Select **Build Windows x64**, choose **Run workflow**, and confirm.
5. When the job finishes, download **HDRMerge-Windows-x64** from the
   **Artifacts** section of the run summary.

The artifact contains both `hdrmerge.exe` (GUI) and `hdrmerge-nogui.exe`
(console mode), the Qt platform plugin, and their runtime DLL dependencies.

## Notes

- Builds run manually and after relevant pushes to `main` or `master`.
- Artifacts expire after seven days to keep storage usage small.
- ALGLIB 3.15.0 GPL is downloaded from its official site during the build.
- The workflow uses a standard `windows-latest` runner; it does not request a
  paid larger runner.
