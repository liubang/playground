#include "dimension.h"
#include "z3.h"

#include <memory>

namespace pl::curve {

enum class TimePeriod {
    Day,
    Week,
    Month,
    Year,
};

class BinnedTime {
public:
    static constexpr uint64_t max_offset(TimePeriod tp) {
        switch (tp) {
        case TimePeriod::Day:
            return 86400000ULL;
        case TimePeriod::Week:
            return 604800ULL;
        case TimePeriod::Month:
            return 2678400;
        case TimePeriod::Year:
        default:
            return 527050;
        }
    }
};

class Z3SFC {
public:
    Z3SFC(TimePeriod period, uint32_t precision) {
        lon_ = std::make_unique<NormalizedLon>(precision);
        lat_ = std::make_unique<NormalizedLat>(precision);
        time_ = std::make_unique<NormalizedTime>(precision, BinnedTime::max_offset(period));
    }

    uint64_t index(double x, double y, uint64_t t) {
        return Z3(lon_->normalize(x), lat_->normalize(y), time_->normalize(t)).val();
    }

private:
    std::unique_ptr<NormalizedDimension> lon_;
    std::unique_ptr<NormalizedDimension> lat_;
    std::unique_ptr<NormalizedDimension> time_;
};

} // namespace pl::curve
