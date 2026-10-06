import AppKit

@MainActor
enum AboutPanel {
    static func show() {
        let paragraphStyle = NSMutableParagraphStyle()
        paragraphStyle.alignment = .center
        let attributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: NSFont.smallSystemFontSize),
            .foregroundColor: NSColor.labelColor,
            .paragraphStyle: paragraphStyle,
        ]
        let credits = NSMutableAttributedString(
            string: String(localized: "Read and write ext2/3/4, XFS, and Btrfs drives on your Mac with Apple FSKit and LKL. No macFUSE or kernel extension required."),
            attributes: attributes
        )
        credits.append(NSAttributedString(string: "\n\n", attributes: attributes))

        var linkAttributes = attributes
        linkAttributes[.link] = "https://github.com/HuanchuanTech/xlinuxfs"
        credits.append(NSAttributedString(
            string: "GitHub: HuanchuanTech/xlinuxfs",
            attributes: linkAttributes
        ))

        NSApplication.shared.orderFrontStandardAboutPanel(options: [.credits: credits])
    }
}
