#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Slicing.hpp"
#include "Slic3r/Biz/Algorithms/LayerHeight.hpp"
#include "Slic3r/Biz/Algorithms/ModelObject.hpp"
#include "Slic3r/Biz/Algorithms/TriangleMesh.hpp"

#include "fff_print/test_data.hpp"

#include <vector>

using Catch::Approx;

using Slic3r::Domain::LayerZRanges;
using Slic3r::Domain::Model;
using Slic3r::Domain::ModelObject;
using Slic3r::Domain::ZHeightPairs;
using Slic3r::SlicingParameters;
using Slic3r::Biz::Algorithms::LayerHeight::GenerateLayersParams;
using Slic3r::Biz::Algorithms::LayerHeight::generate_object_layers;
using Slic3r::Biz::Algorithms::ModelObject::add_volume;
using Slic3r::Biz::Algorithms::TriangleMesh::make_cube;
using Slic3r::Test::TestConfig;

namespace {

SlicingParameters make_slicing_params(
    const double     object_min_z,
    const double     object_max_z,
    TestConfig&      config,
    const ModelObject& object
)
{
    const std::vector<unsigned> extruders{0};
    return SlicingParameters::create_from_config(
        config,
        object.object_settings,
        object_min_z,
        object_max_z,
        extruders,
        config.hw_config
    );
}

} // namespace

// An object stacked on another one ("Paste on top") is lifted off the bed, so its slicing
// parameters must describe its own geometry span instead of a span starting at the bed.
TEST_CASE("Slicing parameters cover the object's own geometry", "[Slicing]")
{
    TestConfig   config{1};
    Model        model;
    ModelObject* object = model.add_object();
    add_volume(object, make_cube(10, 10, 10));
    object->add_instance();

    SECTION("Object stacked on another object (min Z above the bed)")
    {
        constexpr double object_min_z = 20.;
        constexpr double object_max_z = 30.;

        const SlicingParameters sp = make_slicing_params(object_min_z, object_max_z, config, *object);

        CHECK(sp.valid);
        CHECK(sp.object_min_z == Approx(object_min_z));
        CHECK(sp.object_print_z_min == Approx(object_min_z));
        CHECK(sp.object_print_z_max == Approx(object_max_z));
        // Layers span the object's own height, not from the bed up to the object's top.
        CHECK(sp.object_print_z_height() == Approx(object_max_z - object_min_z));
        CHECK(sp.object_print_z_uncompensated_height() == Approx(object_max_z - object_min_z));
    }

    SECTION("Object resting on the bed keeps the previous behaviour")
    {
        constexpr double object_max_z = 10.;

        const SlicingParameters sp = make_slicing_params(0., object_max_z, config, *object);

        CHECK(sp.valid);
        CHECK(sp.object_min_z == Approx(0.));
        CHECK(sp.object_print_z_min == Approx(0.));
        CHECK(sp.object_print_z_max == Approx(object_max_z));
        CHECK(sp.object_print_z_height() == Approx(object_max_z));
    }
}

// The layer grid is generated in the object's own (0-based) coordinate system; the world Z
// is applied afterwards by the slice/print Z offsets, so it must span exactly the height.
TEST_CASE("Object layer grid spans the object height", "[Slicing]")
{
    constexpr double layer_height = 0.2;
    constexpr double height       = 1.0;

    const GenerateLayersParams params{
        .min_layer_height                = 0.1,
        .max_layer_height                = 0.3,
        .first_object_layer_height       = layer_height,
        .object_print_z_height           = height,
        .object_shrinkage_compensation_z = 1.,
        .first_object_layer_height_fixed = false
    };
    const ZHeightPairs profile{{0., layer_height}, {height, layer_height}};

    const LayerZRanges layers = generate_object_layers(params, profile);

    REQUIRE_FALSE(layers.empty());
    CHECK(layers.front().bottom_z == Approx(0.));
    for (const auto& layer : layers) {
        CHECK(layer.bottom_z >= Approx(0.).margin(1e-6));
        CHECK(layer.top_z <= Approx(height).margin(params.max_layer_height + 1e-6));
    }
    CHECK(layers.back().top_z == Approx(height).margin(params.max_layer_height));
}
