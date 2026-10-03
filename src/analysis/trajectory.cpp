#include "analysis/trajectory.hpp"

namespace gucci {

    TrajectoryPredictionService& TrajectoryPredictionService::get() {
        static TrajectoryPredictionService s_service;
        return s_service;
    }

} // namespace gucci
