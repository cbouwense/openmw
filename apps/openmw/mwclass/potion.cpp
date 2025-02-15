#include "potion.hpp"

#include <MyGUI_TextIterator.h>

#include <components/esm3/loadalch.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwbase/windowmanager.hpp"

#include "../mwworld/ptr.hpp"
#include "../mwworld/actionapply.hpp"
#include "../mwworld/cellstore.hpp"

#include "../mwgui/tooltips.hpp"

#include "../mwrender/objects.hpp"
#include "../mwrender/renderinginterface.hpp"

#include "../mwmechanics/alchemy.hpp"

#include "classmodel.hpp"

namespace MWClass
{
    Potion::Potion()
        : MWWorld::RegisteredClass<Potion>(ESM::Potion::sRecordId)
    {
    }

    void Potion::insertObjectRendering (const MWWorld::Ptr& ptr, const std::string& model, MWRender::RenderingInterface& renderingInterface) const
    {
        if (!model.empty()) {
            renderingInterface.getObjects().insertModel(ptr, model);
        }
    }

    std::string Potion::getModel(const MWWorld::ConstPtr &ptr) const
    {
        return getClassModel<ESM::Potion>(ptr);
    }

    std::string_view Potion::getName(const MWWorld::ConstPtr& ptr) const
    {
        const MWWorld::LiveCellRef<ESM::Potion> *ref = ptr.get<ESM::Potion>();
        const std::string& name = ref->mBase->mName;

        return !name.empty() ? name : ref->mBase->mId;
    }

    std::unique_ptr<MWWorld::Action> Potion::activate (const MWWorld::Ptr& ptr,
        const MWWorld::Ptr& actor) const
    {
        return defaultItemActivate(ptr, actor);
    }

    std::string_view Potion::getScript(const MWWorld::ConstPtr& ptr) const
    {
        const MWWorld::LiveCellRef<ESM::Potion> *ref =
            ptr.get<ESM::Potion>();

        return ref->mBase->mScript;
    }

    int Potion::getValue (const MWWorld::ConstPtr& ptr) const
    {
        const MWWorld::LiveCellRef<ESM::Potion> *ref = ptr.get<ESM::Potion>();

        return ref->mBase->mData.mValue;
    }

    std::string_view Potion::getUpSoundId(const MWWorld::ConstPtr& ptr) const
    {
        return "Item Potion Up";
    }

    std::string_view Potion::getDownSoundId(const MWWorld::ConstPtr& ptr) const
    {
        return "Item Potion Down";
    }

    std::string Potion::getInventoryIcon (const MWWorld::ConstPtr& ptr) const
    {
        const MWWorld::LiveCellRef<ESM::Potion> *ref = ptr.get<ESM::Potion>();

        return ref->mBase->mIcon;
    }

    MWGui::ToolTipInfo Potion::getToolTipInfo (const MWWorld::ConstPtr& ptr, int count) const
    {
        const MWWorld::LiveCellRef<ESM::Potion> *ref = ptr.get<ESM::Potion>();

        MWGui::ToolTipInfo info;
        std::string_view name = getName(ptr);
        info.caption = MyGUI::TextIterator::toTagsString({name.data(), name.size()}) + MWGui::ToolTips::getCountString(count);
        info.icon = ref->mBase->mIcon;

        std::string text;

        text += "\n#{sWeight}: " + MWGui::ToolTips::toString(ref->mBase->mData.mWeight);
        text += MWGui::ToolTips::getValueString(ref->mBase->mData.mValue, "#{sValue}");

        info.effects = MWGui::Widgets::MWEffectList::effectListFromESM(&ref->mBase->mEffects);

        // hide effects the player doesn't know about
        MWWorld::Ptr player = MWBase::Environment::get().getWorld ()->getPlayerPtr();
        for (unsigned int i=0; i<info.effects.size(); ++i)
            info.effects[i].mKnown = MWMechanics::Alchemy::knownEffect(i, player);

        info.isPotion = true;

        if (MWBase::Environment::get().getWindowManager()->getFullHelp()) {
            text += MWGui::ToolTips::getCellRefString(ptr.getCellRef());
            text += MWGui::ToolTips::getMiscString(ref->mBase->mScript, "Script");
        }

        info.text = text;

        return info;
    }

    std::unique_ptr<MWWorld::Action> Potion::use (const MWWorld::Ptr& ptr, bool force) const
    {
        MWWorld::LiveCellRef<ESM::Potion> *ref =
            ptr.get<ESM::Potion>();

        std::unique_ptr<MWWorld::Action> action (
            new MWWorld::ActionApply (ptr, ref->mBase->mId));

        action->setSound ("Drink");

        return action;
    }

    MWWorld::Ptr Potion::copyToCellImpl(const MWWorld::ConstPtr &ptr, MWWorld::CellStore &cell) const
    {
        const MWWorld::LiveCellRef<ESM::Potion> *ref = ptr.get<ESM::Potion>();

        return MWWorld::Ptr(cell.insert(ref), &cell);
    }

    bool Potion::canSell (const MWWorld::ConstPtr& item, int npcServices) const
    {
        return (npcServices & ESM::NPC::Potions) != 0;
    }

    float Potion::getWeight(const MWWorld::ConstPtr &ptr) const
    {
        const MWWorld::LiveCellRef<ESM::Potion> *ref = ptr.get<ESM::Potion>();
        return ref->mBase->mData.mWeight;
    }
}
