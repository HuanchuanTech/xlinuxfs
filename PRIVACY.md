# Privacy Policy

Last updated: June 29, 2026

xlinuxfs is an open-source macOS utility for mounting Linux filesystems (ext2/3/4, XFS, Btrfs) with Apple's FSKit file-system extension model and the real Linux kernel via LKL (Linux Kernel Library).

This policy explains what the app and its file-system extension do with data. The short version: xlinuxfs works locally on your Mac. It does not run ads, does not track you, and does not upload your files or device information to our servers.

## Data Collection

xlinuxfs does not collect personal data.

The app does not include analytics, advertising SDKs, tracking SDKs, user accounts, telemetry, or in-app network reporting.

## Local File and Disk Access

xlinuxfs needs access to Linux filesystem volumes in order to provide file-system functionality.

When a Linux drive or disk image is mounted through the xlinuxfs FSKit extension, the extension may read and write file-system data on that volume, including file contents, file names, directory entries, file metadata, and on-disk filesystem structures (ext/XFS/Btrfs). This processing happens locally on your Mac as part of normal file-system operation.

xlinuxfs does not upload the contents of your drives, file names, directory listings, or disk images.

## Disk Images

If you choose a disk image in the app, macOS grants the app access to that selected file. xlinuxfs uses the selected path to show a copyable command for attaching the image. The app does not upload the image file.

## Diagnostics

xlinuxfs includes a diagnostics view to help explain whether the FSKit extension is installed, enabled, or registered more than once.

Diagnostics may show local status information such as:

- whether the extension is installed
- whether the extension is enabled
- local extension registration paths
- suggested Terminal commands for troubleshooting

This information is shown locally in the app. xlinuxfs does not send it to us automatically.

## Clipboard

Some operations are shown as copyable Terminal commands because the macOS sandbox prevents the app from running those commands directly. When you click a Copy button, xlinuxfs writes that command to the macOS clipboard.

## Settings

xlinuxfs stores simple local preferences, such as whether disk drives and disk images should mount read-only by default. These preferences are stored locally using macOS user defaults in an App Group container shared with the file-system extension, so the extension can apply them when a volume mounts.

## Network Access

xlinuxfs does not need a network connection for normal operation and does not send app data to a remote service.

If you visit the project website, GitHub repository, or support pages, those services may process data according to their own privacy policies.

## Third-Party Code

xlinuxfs uses the Linux kernel (via LKL, the Linux Kernel Library) and its ext4/XFS/Btrfs drivers as its filesystem engine. This code runs locally as part of the file-system implementation included with the app extension, and is licensed under the GNU General Public License, version 2.

## Crash Reports and System Logs

macOS may create crash reports or system logs according to your system settings. xlinuxfs does not automatically receive or upload those reports. If you choose to share logs or diagnostics in a GitHub issue or support request, please review them first and remove anything you do not want to share.

## Children

xlinuxfs is a general-purpose utility and does not knowingly collect data from children.

## Open Source

The source code is available at:

https://github.com/HuanchuanTech/xlinuxfs

## Changes

We may update this policy when the app changes. The latest version will be published with the app and in the project repository.

## Contact

For questions or privacy-related requests, please use the project repository:

https://github.com/HuanchuanTech/xlinuxfs
