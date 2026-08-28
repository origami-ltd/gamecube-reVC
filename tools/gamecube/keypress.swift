// Post a real HID-level key press (down, hold, up). Dolphin's Quartz/IOHID
// keyboard backend ignores System Events synthetics; CGEventPost at the HID
// tap is what its polling actually sees.
//   swift keypress.swift <keycode> [holdMs=120]
import CoreGraphics
import Foundation

let args = CommandLine.arguments
guard args.count >= 2, let codeNum = UInt16(args[1]) else {
	FileHandle.standardError.write("usage: keypress <keycode> [holdMs]\n".data(using: .utf8)!)
	exit(2)
}
let holdMs = args.count > 2 ? (Double(args[2]) ?? 120) : 120
let src = CGEventSource(stateID: .hidSystemState)
CGEvent(keyboardEventSource: src, virtualKey: CGKeyCode(codeNum), keyDown: true)?
	.post(tap: .cghidEventTap)
usleep(useconds_t(holdMs * 1000))
CGEvent(keyboardEventSource: src, virtualKey: CGKeyCode(codeNum), keyDown: false)?
	.post(tap: .cghidEventTap)
