#!/usr/bin/env swift
// =============================================================================
//  render_store_images.swift — Play Store listing images, drawn from the mark.
//
//  The launcher icon is a vector (res/drawable/ic_launcher_*.xml): five
//  tangerine bars with round caps on the project ink, in a 108-unit viewport.
//  Play wants raster images instead, so this draws the same geometry with
//  CoreGraphics rather than resampling a bitmap — every size stays sharp, and
//  the three platforms' icons cannot drift apart.
//
//  Usage:  swift scripts/render_store_images.swift
//  Writes: apps/android-compose/playstore/metadata/android/en-US/images/
//            icon.png            512 × 512, 32-bit PNG (Play's hi-res icon)
//            featureGraphic.png  1024 × 500 (required to publish a listing)
//
//  No text is drawn: the system fonts' licences do not clearly cover store
//  artwork, and a wordmark belongs in a deliberate brand decision.
// =============================================================================
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let ink = CGColor(srgbRed: 0x2d / 255, green: 0x31 / 255, blue: 0x42 / 255, alpha: 1)
let tangerine = CGColor(srgbRed: 0xeb / 255, green: 0x6c / 255, blue: 0x36 / 255, alpha: 1)

/// The launcher icon's bars, in its 108-unit viewport: x, top, bottom. Stroke
/// width 8 with round caps, as in ic_launcher_foreground.xml.
let bars: [(x: CGFloat, top: CGFloat, bottom: CGFloat)] = [
    (30, 44, 64), (42, 36, 72), (54, 28, 80), (66, 38, 70), (78, 46, 62),
]
let strokeWidth: CGFloat = 8

func context(width: Int, height: Int) -> CGContext {
    let context = CGContext(
        data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0,
        space: CGColorSpace(name: CGColorSpace.sRGB)!,
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue   // 32-bit RGBA
    )!
    // Flip to a top-left origin, matching the vector's coordinate system.
    context.translateBy(x: 0, y: CGFloat(height))
    context.scaleBy(x: 1, y: -1)
    return context
}

func drawBar(_ context: CGContext, x: CGFloat, top: CGFloat, bottom: CGFloat,
             width: CGFloat, color: CGColor) {
    context.setStrokeColor(color)
    context.setLineWidth(width)
    context.setLineCap(.round)
    context.move(to: CGPoint(x: x, y: top))
    context.addLine(to: CGPoint(x: x, y: bottom))
    context.strokePath()
}

func write(_ context: CGContext, to path: String) {
    let url = URL(fileURLWithPath: path)
    try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                             withIntermediateDirectories: true)
    let destination = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(destination, context.makeImage()!, nil)
    guard CGImageDestinationFinalize(destination) else { fatalError("could not write \(path)") }
    print("wrote \(path)")
}

let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().path
let out = "\(root)/apps/android-compose/playstore/metadata/android/en-US/images"

// -- Hi-res icon: the launcher icon exactly, full bleed. Play applies its own
//    rounded mask, so the background fills the square.
do {
    let size = 512
    let scale = CGFloat(size) / 108
    let c = context(width: size, height: size)
    c.setFillColor(ink)
    c.fill(CGRect(x: 0, y: 0, width: size, height: size))
    for bar in bars {
        drawBar(c, x: bar.x * scale, top: bar.top * scale, bottom: bar.bottom * scale,
                width: strokeWidth * scale, color: tangerine)
    }
    write(c, to: "\(out)/icon.png")
}

// -- Feature graphic: the mark as a waveform running the width of the banner,
//    the icon's five bars at full strength in the middle, echoes fading out
//    towards the edges.
do {
    let width = 1024, height = 500
    let c = context(width: width, height: height)
    c.setFillColor(ink)
    c.fill(CGRect(x: 0, y: 0, width: width, height: height))

    let scale: CGFloat = 5                       // icon units → pixels
    let pitch: CGFloat = 12 * scale              // bar spacing in the icon
    let midX = CGFloat(width) / 2, midY = CGFloat(height) / 2
    // Half-heights of the icon's bars, repeated outwards and shrinking.
    let halves: [CGFloat] = bars.map { ($0.bottom - $0.top) / 2 }
    let count = Int(CGFloat(width) / pitch) + 2
    for i in -count / 2...count / 2 {
        let distance = abs(i)
        let base = halves[(i + 2 + 5 * 100) % 5]
        let fade = distance <= 2 ? 1 : max(0.12, 1 - CGFloat(distance - 2) * 0.13)
        let half = base * (distance <= 2 ? 1 : max(0.35, 1 - CGFloat(distance - 2) * 0.07))
        let color = tangerine.copy(alpha: fade)!
        let x = midX + CGFloat(i) * pitch
        drawBar(c, x: x, top: midY - half * scale, bottom: midY + half * scale,
                width: strokeWidth * scale, color: color)
    }
    write(c, to: "\(out)/featureGraphic.png")
}
