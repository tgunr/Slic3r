#pragma once

#include "Slic3r/Domain/BedRef.hpp"
#include "Slic3r/Domain/ElementRef.hpp"
#include "Slic3r/Domain/SelectionId.hpp"

#include <optional>
#include <vector>

namespace Slic3r::Biz {
class ArrangeInteractor;
} // namespace Slic3r::Biz

namespace Slic3r::Biz::Scene {
class SceneInteractor;
enum class SelectionMode;
} // namespace Slic3r::Biz::Scene

namespace Slic3r::Domain {
class ModelObject;
class Workbench;
} // namespace Slic3r::Domain

namespace Slic3r::Biz {

class ClipboardInteractor
{
public:
    ClipboardInteractor() = delete;
    ClipboardInteractor(
        Scene::SceneInteractor& scene_interactor,
        ArrangeInteractor& arrange_interactor,
        const Domain::Workbench& workbench
    );
    ~ClipboardInteractor() = default;

    /**
     * @brief Returns true if the current selection is non-empty and can be copied.
     */
    bool can_copy() const;

    /**
     * @brief Returns true if the clipboard has valid content that can be pasted.
     */
    bool can_paste() const;

    /**
     * @brief Stores the current selection for later pasting.
     * @param project_id ID of the project containing the selected objects.
     */
    void copy(Domain::SelectionId project_id);

    /**
     * @brief Pastes stored selection into the currently selected project.
     * @param project_id ID of the target project to paste into.
     */
    void paste(Domain::SelectionId project_id);

    /**
     * @brief Returns true when the clipboard holds exactly one object instance that
     * can be pasted on top of an existing object in the same project.
     */
    bool can_paste_on_top() const;

    /**
     * @brief Number of material slots (extruders) available on the target bed of the
     * given project, or 0 when it cannot be determined.
     */
    int target_interface_extruder_count(Domain::SelectionId project_id) const;

    /**
     * @brief Extruder override currently assigned to the source object's top layer, if
     * any. Used as the default value of the interface extruder prompt.
     */
    std::optional<int> source_top_interface_extruder() const;

    /**
     * @brief True while a stack paste sequence is in progress for the current clipboard,
     * i.e. the source/clone already carries the interface configuration.
     */
    bool is_stack_active() const;

    /**
     * @brief Pastes the stored object on top of the source object (or of the
     * previously pasted clone) instead of arranging it on the bed.
     * @param project_id ID of the target project to paste into.
     * @param interface_extruder Optional 1-based extruder index assigned to the
     * interface layer (the lower object's top layer). std::nullopt leaves the
     * existing interface configuration untouched.
     */
    void paste_on_top(Domain::SelectionId project_id, std::optional<int> interface_extruder);

private:
    struct Clipboard
    {
        Domain::SelectionId source_project_id{Domain::INVALID_ID};
        Scene::SelectionMode mode{};
        Domain::ElementRefs selected_elements;

        bool is_empty() const;
        void clear();
    };

    void paste_objects(Domain::SelectionId project_id);
    void paste_volumes(Domain::SelectionId project_id);
    void paste_objects_on_top(Domain::SelectionId project_id, std::optional<int> interface_extruder);

    /**
     * @brief Object referenced by the clipboard, or nullptr when it no longer exists.
     */
    const Domain::ModelObject* source_object() const;

    Scene::SceneInteractor& m_scene_interactor;
    ArrangeInteractor& m_arrange_interactor;
    const Domain::Workbench& m_workbench;

    Clipboard m_clipboard;

    // World Z of the top of the current stack, advanced by every stack paste and
    // reset when the clipboard content changes.
    std::optional<double> m_stack_cursor;
};

} // namespace Slic3r::Biz
