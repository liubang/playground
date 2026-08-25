import SwiftUI

/// Precomputed, Equatable view model for a day cell. Built once per
/// displayed month (see CalendarPopover.rebuildCells) so re-renders never
/// redo calendar math or lunar conversion.
struct DayCellData: Equatable, Identifiable, Sendable {
    let date: Date
    let day: Int
    let isToday: Bool
    let isInDisplayedMonth: Bool
    /// Rest-style red coloring: holidays and weekends, unless shifted workday.
    let isRestColored: Bool
    let subtitle: String
    /// Subtitle names a statutory festival — render in rest red.
    let isFestival: Bool
    /// Subtitle names a solar term — render in teal.
    let isTerm: Bool
    let badge: Holidays.Kind?
    /// The day has at least one system-calendar event — a tiny dot.
    let hasEvent: Bool

    var id: Date {
        date
    }
}

/// A single day cell: large Gregorian day number on top, tiny lunar /
/// solar-term / festival subtitle below, and an optional 休/班 corner
/// badge. Equatable by `data` and `isSelected` — unchanged cells cost
/// nothing on re-render.
struct DayCell: View, Equatable {
    let data: DayCellData
    let isSelected: Bool
    let action: () -> Void

    @Environment(\.theme) private var theme

    static func == (lhs: DayCell, rhs: DayCell) -> Bool {
        lhs.data == rhs.data && lhs.isSelected == rhs.isSelected
    }

    private var numberColor: Color {
        if data.isToday {
            return theme.background
        }
        if !data.isInDisplayedMonth {
            return theme.textSecondary.opacity(0.45)
        }
        if data.isRestColored {
            return theme.rest.opacity(0.85)
        }
        return theme.textPrimary
    }

    private var subtitleColor: Color {
        if !data.isInDisplayedMonth {
            return theme.textSecondary.opacity(0.4)
        }
        if data.isFestival {
            return theme.rest
        }
        if data.isTerm {
            return theme.accent
        }
        return theme.textSecondary.opacity(0.9)
    }

    var body: some View {
        Button(action: action) {
            VStack(spacing: 1) {
                Text("\(data.day)")
                    .font(.system(.callout, design: .rounded).weight(data.isToday ? .bold : .regular))
                    .monospacedDigit()
                    .foregroundStyle(numberColor)
                    .frame(width: 24, height: 24)
                    .background {
                        if data.isToday {
                            Circle().fill(theme.accent)
                        }
                    }
                Text(data.subtitle)
                    .font(.system(size: 8))
                    .foregroundStyle(subtitleColor)
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
            }
            .frame(maxWidth: .infinity)
            .frame(height: 40)
            .padding(.bottom, 3)
            .background {
                if isSelected, !data.isToday {
                    RoundedRectangle(cornerRadius: 7)
                        .stroke(theme.cardBorder, lineWidth: 1)
                }
            }
            .overlay(alignment: .bottom) {
                if data.hasEvent {
                    Circle()
                        .fill(theme.accent.opacity(0.9))
                        .frame(width: 3, height: 3)
                        .padding(.bottom, 1.5)
                }
            }
            .overlay(alignment: .topTrailing) {
                if let badge = data.badge {
                    Text(badge.badge)
                        .font(.system(size: 7, weight: .bold))
                        .foregroundStyle(theme.background)
                        .padding(.horizontal, 2.5)
                        .padding(.vertical, 1)
                        .background(
                            RoundedRectangle(cornerRadius: 3)
                                .fill(badge == .rest ? theme.rest : theme.warning),
                        )
                        .padding(.top, 1)
                        .padding(.trailing, 1)
                }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}
