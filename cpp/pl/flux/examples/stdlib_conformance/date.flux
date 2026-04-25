import "date"

t = 2024-02-03T04:05:06Z

{
    year: date.year(t: t),
    month: date.month(t: t),
    day: date.monthDay(t: t),
    weekday: date.weekDay(t: t),
    hour: date.hour(t: t),
    minute: date.minute(t: t),
    second: date.second(t: t),
    truncated: string(v: date.truncate(t: t, unit: 1h)),
    added: string(v: date.add(d: 2h, to: t)),
    subtracted: string(v: date.sub(d: 5m, from: t)),
}

