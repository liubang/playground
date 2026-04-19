option task = {name: "feature-gallery", every: 5m}

env = "prod"
services = ["api", "worker", "scheduler"]
regions = {east: "us-east", west: "us-west"}
loads = [71.0, 88.0, 64.0]
serviceCount = len(services)
fallbackRegion = if exists regions.central then regions.central else "missing"
label = "fleet ${serviceCount}"

{
    env: env,
    trackedApi: contains(value: "api", set: services),
    serviceCount: serviceCount,
    regionCount: len(regions),
    firstService: services[0],
    eastRegion: regions.east,
    fallbackRegion: fallbackRegion,
    label: label,
    totalLoad: sum(loads),
    averageLoad: mean(loads),
    minimumLoad: min(loads),
    maximumLoad: max(loads),
}
