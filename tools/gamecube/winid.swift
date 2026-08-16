// Print Dolphin's render window id. .optionAll (not onScreenOnly) because
// Dolphin's window can sit on another Space and would otherwise be invisible
// to CGWindowList.
import CoreGraphics
import Foundation

guard let list = CGWindowListCopyWindowInfo(.optionAll, kCGNullWindowID)
        as? [[String: Any]] else { exit(1) }

for w in list {
    let owner = w[kCGWindowOwnerName as String] as? String ?? ""
    guard owner.localizedCaseInsensitiveContains("dolphin") else { continue }
    let num = w[kCGWindowNumber as String] as? Int ?? 0
    let name = w[kCGWindowName as String] as? String ?? ""
    let b = w[kCGWindowBounds as String] as? [String: Any] ?? [:]
    let width = Int(b["Width"] as? Double ?? 0)
    let height = Int(b["Height"] as? Double ?? 0)
    guard width > 200 && height > 200 else { continue }
    print("\(num)\t\(width)x\(height)\t\(owner)\t\(name)")
}
