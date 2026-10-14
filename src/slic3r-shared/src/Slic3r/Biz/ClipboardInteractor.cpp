#include "Slic3r/Biz/ClipboardInteractor.hpp"

#include "Slic3r/Biz/Algorithms/ModelObject.hpp"
#include "Slic3r/Biz/ArrangeInteractor.hpp"
#include "Slic3r/Biz/IUndoProvider.hpp"
#include "Slic3r/Biz/Scene/SceneInteractor.hpp"
#include "Slic3r/Domain/ConfigContainer.hpp"
#include "Slic3r/Domain/ConfigPack.hpp"
#include "Slic3r/Domain/LayerHeightProfile.hpp"
#include "Slic3r/Domain/Model.hpp"
#include "Slic3r/Domain/ModelInstance.hpp"
#include "Slic3r/Domain/ModelObject.hpp"
#include "Slic3r/Domain/ModelVolume.hpp"
#include "Slic3r/Domain/Preset/SelectedPresetConfigPack.hpp"
#include "Slic3r/Domain/Transformation.hpp"
#include "Slic3r/Domain/Workbench.hpp"

#include <cmath>
#include <set>

using namespace Slic3r;
using namespace Slic3r::Biz;

using Slic3r::Biz::Scene::ObjectSelection;
using Slic3r::Biz::Scene::SelectionMode;
using Slic3r::Domain::BedRef;
using Slic3r::Domain::ConfigContainer;
using Slic3r::Domain::ElementRef;
using Slic3r::Domain::ElementRefs;
using Slic3r::Domain::LayerConfigRanges;
using Slic3r::Domain::LayerHeightRange;
using Slic3r::Domain::ModelInstance;
using Slic3r::Domain::ModelObject;
using Slic3r::Domain::ModelObjectPtrs;
using Slic3r::Domain::ModelVolume;
using Slic3r::Domain::Project;
using Slic3r::Domain::SelectionId;
using Slic3r::Domain::VolumeSettings;
using Slic3r::Domain::Workbench;

namespace {
// Tolerance used when matching a Z height against the boundary of a height range.
constexpr double STACK_INTERFACE_EPSILON = 1e-4;

// Assigns the given extruder to the height range covering the object's top layer,
// adding the extruder to an existing range or inserting a one-layer range. Does not
// overwrite an already specified extruder override of an existing range.
void set_top_interface_range(ModelObject& object, int extruder, double top_z, double layer_height)
{
    for (auto& [range, settings] : object.layer_config_ranges) {
        const bool covers_top = range.first - STACK_INTERFACE_EPSILON <= top_z
                                && top_z <= range.second + STACK_INTERFACE_EPSILON;
        if (covers_top) {
            settings.overrides.set("extruder", extruder);
            return;
        }
    }

    const double bottom_z = top_z - std::max(layer_height, STACK_INTERFACE_EPSILON);
    VolumeSettings settings;
    settings.overrides.set("extruder", extruder);
    object.layer_config_ranges.emplace(LayerHeightRange{bottom_z, top_z}, std::move(settings));
}

void shift_ranges_by(LayerConfigRanges& ranges, double dz)
{
    LayerConfigRanges shifted;
    for (auto& [range, settings] : ranges) {
        shifted.emplace(
            LayerHeightRange{range.first + dz, range.second + dz},
            std::move(settings)
        );
    }
    ranges = std::move(shifted);
}
} // namespace

namespace Slic3r::Biz {

ClipboardInteractor::ClipboardInteractor(
    Scene::SceneInteractor& scene_interactor,
    ArrangeInteractor& arrange_interactor,
    const Workbench& workbench
) :
    m_scene_interactor{scene_interactor},
    m_arrange_interactor{arrange_interactor},
    m_workbench{workbench}
{}

bool ClipboardInteractor::can_copy() const
{
    const ObjectSelection& object_selection = m_scene_interactor.object_selection();
    return !object_selection.empty() && !object_selection.contains_wipe_tower();
}

bool ClipboardInteractor::can_paste() const
{
    if (m_clipboard.is_empty()) {
        return false;
    } else if (!m_workbench.projects().contains(m_clipboard.source_project_id)) {
        return false;
    } else if (m_clipboard.mode == SelectionMode::Volume
               && !m_scene_interactor.object_selection().only_single_object())
    {
        // We allow pasting volume only when single object is sellected.
        return false;
    }

    const Project& source_project = m_workbench.project(m_clipboard.source_project_id);
    const bool any_source_exists  = std::ranges::any_of(
        m_clipboard.selected_elements,
        [&source_project](const ElementRef& element)
        { return source_project.find_object_by_id(element.object_id) != nullptr; }
    );

    return any_source_exists;
}

void ClipboardInteractor::copy(const SelectionId project_id)
{
    const ObjectSelection& selection = m_scene_interactor.object_selection();
    if (selection.empty()) {
        return;
    }

    m_clipboard.clear();
    m_clipboard.source_project_id = project_id;
    m_clipboard.mode              = selection.mode;
    m_clipboard.selected_elements = selection.elements;
    m_stack_cursor.reset();
}

void ClipboardInteractor::paste(const SelectionId project_id)
{
    if (m_clipboard.is_empty()) {
        return;
    }

    if (m_clipboard.mode == SelectionMode::Volume) {
        this->paste_volumes(project_id);
    } else {
        this->paste_objects(project_id);
    }
}

void ClipboardInteractor::Clipboard::clear()
{
    this->source_project_id = Domain::INVALID_ID;
    this->mode              = SelectionMode::Instance;
    this->selected_elements.clear();
}

bool ClipboardInteractor::Clipboard::is_empty() const
{
    return selected_elements.empty();
}

void ClipboardInteractor::paste_objects(const SelectionId project_id)
{
    const ModelObjectPtrs new_objects = m_scene_interactor.clone_objects_from_project(
        m_clipboard.source_project_id,
        m_clipboard.selected_elements
    );

    ElementRefs new_instances;
    for (const ModelObject* new_object : new_objects) {
        for (const ModelInstance* instance : new_object->instances) {
            new_instances.emplace_back(new_object->id().id, instance->id().id, 0);
        }
    }

    if (new_instances.empty()) {
        return;
    }

    const BedRef target_bed = m_scene_interactor.bed_selection().last_selected_bed();
    m_arrange_interactor.arrange_added_instances(
        project_id,
        new_instances,
        target_bed,
        UndoSnapshotType::PasteObjects
    );
}

void ClipboardInteractor::paste_volumes(SelectionId project_id)
{
    ASSERT(m_clipboard.mode == SelectionMode::Volume);
    ASSERT(!m_clipboard.selected_elements.empty());

    const ObjectSelection& selection = m_scene_interactor.object_selection();
    if (selection.empty() || !selection.only_single_object()) {
        return;
    }

    const Project& source_project    = m_workbench.project(m_clipboard.source_project_id);
    const size_t source_object_id    = m_clipboard.selected_elements.front().object_id;
    const ModelObject* source_object = source_project.find_object_by_id(source_object_id);
    if (source_object == nullptr) {
        return;
    }

    std::set<size_t> selected_volume_ids;
    for (const ElementRef& selected_element : m_clipboard.selected_elements) {
        if (selected_element.has_volume()) {
            selected_volume_ids.insert(selected_element.volume_id);
        }
    }

    if (selected_volume_ids.empty()) {
        return;
    }

    // Pasting into the source object modifies its volume list, so iterate over a copy.
    const Domain::ModelVolumePtrs source_volumes = source_object->volumes;
    const size_t selection_instance_id           = selection.elements.front().instance_id;
    for (const ModelVolume* source_volume : source_volumes) {
        if (!selected_volume_ids.contains(source_volume->id().id)) {
            continue;
        }

        m_scene_interactor.add_volume(
            project_id,
            selection_instance_id,
            [source_volume](ModelObject& target_object) -> ModelVolume*
            {
                ModelVolume* new_volume = target_object.add_volume(*source_volume);
                new_volume->set_new_unique_id();
                return new_volume;
            }
        );
    }

    m_scene_interactor.undo_provider().take_snapshot(UndoSnapshotType::PasteVolumes);
}

bool ClipboardInteractor::can_paste_on_top() const
{
    if (m_clipboard.is_empty()) {
        return false;
    }
    if (m_clipboard.mode != SelectionMode::Instance) {
        return false;
    }
    if (m_clipboard.selected_elements.size() != 1) {
        return false;
    }
    if (m_clipboard.selected_elements.front().is_wipe_tower()) {
        return false;
    }

    const ModelObject* object = source_object();
    return object != nullptr && !object->instances.empty();
}

const ModelObject* ClipboardInteractor::source_object() const
{
    if (m_clipboard.selected_elements.size() != 1) {
        return nullptr;
    }

    const Project* source_project = m_workbench.find_project_by_id(m_clipboard.source_project_id);
    if (source_project == nullptr) {
        return nullptr;
    }

    return source_project->find_object_by_id(m_clipboard.selected_elements.front().object_id);
}

int ClipboardInteractor::target_interface_extruder_count(const SelectionId project_id) const
{
    const Project* project = m_workbench.find_project_by_id(project_id);
    if (project == nullptr) {
        return 0;
    }

    const BedRef target_bed = m_scene_interactor.bed_selection().last_selected_bed();
    const ConfigContainer* config_container =
        project->find_config_container(target_bed.config_container_id);

    // Fall back to the first FFF config container when the selected bed's container
    // cannot be resolved.
    if (config_container == nullptr) {
        for (const auto& candidate : project->config_containers()) {
            if (candidate != nullptr
                && candidate->print_technology() == Domain::PrinterTechnology::FFF)
            {
                config_container = candidate.get();
                break;
            }
        }
    }

    if (config_container == nullptr
        || config_container->print_technology() != Domain::PrinterTechnology::FFF)
    {
        return 0;
    }

    const Domain::Preset::SelectedPresetConfigPack preset_config_pack(
        config_container->selected_preset()
    );
    return static_cast<int>(preset_config_pack.filament_size());
}

std::optional<int> ClipboardInteractor::source_top_interface_extruder() const
{
    const ModelObject* object = source_object();
    if (object == nullptr || object->instances.empty()) {
        return std::nullopt;
    }

    const double top_z = object->max_z();
    for (const auto& [range, settings] : object->layer_config_ranges) {
        const bool covers_top = range.first - STACK_INTERFACE_EPSILON <= top_z
                                && top_z <= range.second + STACK_INTERFACE_EPSILON;
        if (covers_top) {
            if (const std::optional<Domain::ConfigItem> item = settings.overrides.get("extruder")) {
                return item->get<int>();
            }
        }
    }

    return std::nullopt;
}

bool ClipboardInteractor::is_stack_active() const
{
    return m_stack_cursor.has_value();
}

void ClipboardInteractor::paste_on_top(const SelectionId project_id, std::optional<int> interface_extruder)
{
    if (!can_paste_on_top()) {
        return;
    }

    this->paste_objects_on_top(project_id, interface_extruder);
}

void ClipboardInteractor::paste_objects_on_top(
    const SelectionId project_id,
    std::optional<int> interface_extruder
)
{
    const ElementRef source_element = m_clipboard.selected_elements.front();
    const Project& source_project   = m_workbench.project(m_clipboard.source_project_id);
    const ModelObject* source_object_ptr = source_project.find_object_by_id(source_element.object_id);
    if (source_object_ptr == nullptr || source_object_ptr->instances.empty()) {
        return;
    }

    const ModelInstance* source_instance =
        source_project.find_instance_by_id(source_element.object_id, source_element.instance_id);
    if (source_instance == nullptr) {
        source_instance = source_object_ptr->instances.front();
    }

    const Domain::BoundingBox3d source_bb =
        Algorithms::ModelObject::instance_bounding_box(*source_object_ptr, *source_instance);
    const double source_min_z = source_bb.min.z();
    const double source_top_z = source_bb.max.z();
    const double anchor      = m_stack_cursor.value_or(source_top_z);

    // Effective layer height of the target bed, used for the auto-created interface range.
    double layer_height = 0.2;
    if (const Project* target_project = m_workbench.find_project_by_id(project_id)) {
        const BedRef target_bed = m_scene_interactor.bed_selection().last_selected_bed();
        if (const ConfigContainer* config_container =
                target_project->find_config_container(target_bed.config_container_id))
        {
            const Domain::ConfigPack config_pack = config_container->build_print_config();
            if (const auto* fdm = std::get_if<Domain::ConfigPackFDM>(&config_pack)) {
                layer_height = fdm->print.items.opt("layer_height").get<double>();
            }
        }
    }

    // Assign the interface filament to the lower object's top layer, so that the clone
    // inherits (and, after the Z shift below, keeps) the interface range on its own top.
    if (interface_extruder.has_value() && *interface_extruder > 0) {
        const int extruder = *interface_extruder;
        const auto apply_interface = [extruder, source_top_z, layer_height](ModelObject& object)
        {
            set_top_interface_range(object, extruder, source_top_z, layer_height);
        };

        if (m_clipboard.source_project_id == project_id) {
            m_scene_interactor.modify_layer_config_ranges(
                ElementRef{source_element.object_id},
                apply_interface
            );
        } else {
            apply_interface(*const_cast<ModelObject*>(source_object_ptr));
        }
    }

    const ModelObjectPtrs new_objects = m_scene_interactor.clone_objects_from_project(
        m_clipboard.source_project_id,
        m_clipboard.selected_elements
    );
    if (new_objects.empty() || new_objects.front() == nullptr) {
        return;
    }

    ModelObject* clone = new_objects.front();
    if (clone->instances.empty()) {
        return;
    }

    const double clone_min_z =
        Algorithms::ModelObject::instance_bounding_box(*clone, *clone->instances.front()).min.z();
    const double dz = anchor - clone_min_z;

    Scene::SceneInteractor::ElementTransforms transforms;
    for (const ModelInstance* instance : clone->instances) {
        Domain::Transform3d translation = Domain::Transform3d::Identity();
        translation.translate(Domain::Vec3d{0., 0., dz});
        const Domain::Transform3d matrix = translation * instance->get_matrix();
        transforms.emplace(
            ElementRef{clone->id().id, instance->id().id, 0},
            matrix.matrix()
        );
    }
    m_scene_interactor.set_element_transforms(transforms);

    if (std::abs(dz) > STACK_INTERFACE_EPSILON) {
        // Height ranges are matched against the world Z of the slicing planes, so move the
        // clone's ranges onto its new (world) position so its own interface range keeps
        // applying. The layer height profile is stored in the object's own (0-based) Z and
        // is inherited unchanged, so it is intentionally not shifted.
        m_scene_interactor.modify_layer_config_ranges(
            ElementRef{clone->id().id},
            [dz](ModelObject& object) { shift_ranges_by(object.layer_config_ranges, dz); }
        );
    }

    ElementRefs new_instances;
    for (const ModelInstance* instance : clone->instances) {
        new_instances.emplace_back(clone->id().id, instance->id().id, 0);
    }
    if (!new_instances.empty()) {
        ObjectSelection selection;
        selection.mode     = SelectionMode::Instance;
        selection.elements = std::move(new_instances);
        m_scene_interactor.set_object_selection(selection);
    }

    // The next stack paste lands on top of this clone.
    m_stack_cursor = anchor + (source_top_z - source_min_z);

    m_scene_interactor.undo_provider().take_snapshot(UndoSnapshotType::PasteObjectsOnTop);
}

} // namespace Slic3r::Biz
