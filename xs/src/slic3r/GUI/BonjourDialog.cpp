#include "slic3r/Utils/Bonjour.hpp"   // On Windows, boost needs to be included before wxWidgets headers

#include "BonjourDialog.hpp"

#include <mutex>

#include <wx/sizer.h>
#include <wx/button.h>
#include <wx/listctrl.h>
#include <wx/stattext.h>
#include <wx/timer.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/Utils/Bonjour.hpp"


namespace Slic3r {


struct BonjourReplyEvent : public wxEvent
{
	BonjourReply reply;

	BonjourReplyEvent(wxEventType eventType, int winid, BonjourReply &&reply) :
		wxEvent(winid, eventType),
		reply(std::move(reply))
	{}

	virtual wxEvent *Clone() const
	{
		return new BonjourReplyEvent(*this);
	}
};

wxDEFINE_EVENT(EVT_BONJOUR_REPLY, BonjourReplyEvent);

wxDECLARE_EVENT(EVT_BONJOUR_COMPLETE, wxCommandEvent);
wxDEFINE_EVENT(EVT_BONJOUR_COMPLETE, wxCommandEvent);

struct LifetimeGuard
{
	std::mutex mutex;
	BonjourDialog *dialog;

	LifetimeGuard(BonjourDialog *dialog) : dialog(dialog) {}
};


BonjourDialog::BonjourDialog(wxWindow *parent) :
	wxDialog(parent, wxID_ANY, _(L("Network lookup"))),
	list(new wxListView(this, wxID_ANY, wxDefaultPosition, wxSize(800, 300))),
	label(new wxStaticText(this, wxID_ANY, "")),
	timer(new wxTimer()),
	timer_state(0)
{
	wxBoxSizer *vsizer = new wxBoxSizer(wxVERTICAL);

	vsizer->Add(label, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, 10);

	list->SetSingleStyle(wxLC_SINGLE_SEL);
	list->SetSingleStyle(wxLC_SORT_DESCENDING);
	list->AppendColumn(_(L("Address")), wxLIST_FORMAT_LEFT, 50);
	list->AppendColumn(_(L("Hostname")), wxLIST_FORMAT_LEFT, 100);
	list->AppendColumn(_(L("Service name")), wxLIST_FORMAT_LEFT, 200);
	list->AppendColumn(_(L("OctoPrint version")), wxLIST_FORMAT_LEFT, 50);

	vsizer->Add(list, 1, wxEXPAND | wxALL, 10);

	wxBoxSizer *button_sizer = new wxBoxSizer(wxHORIZONTAL);
	button_sizer->Add(new wxButton(this, wxID_OK, "OK"), 0, wxALL, 10);
	button_sizer->Add(new wxButton(this, wxID_CANCEL, "Cancel"), 0, wxALL, 10);
	// ^ Note: The Ok/Cancel labels are translated by wxWidgets

	vsizer->Add(button_sizer, 0, wxALIGN_CENTER);
	SetSizerAndFit(vsizer);

	Bind(EVT_BONJOUR_REPLY, &BonjourDialog::on_reply, this);

	Bind(EVT_BONJOUR_COMPLETE, [this](wxCommandEvent &) {
		this->timer_state = 0;
	});

	Bind(wxEVT_TIMER, &BonjourDialog::on_timer, this);
}

BonjourDialog::~BonjourDialog()
{
	// Needed bacuse of forward defs
}

bool BonjourDialog::show_and_lookup()
{
	Show();
	auto this_id = GetId();

	timer->Stop();
	timer->SetOwner(this);
	timer_state = 1;
	timer->Start(1000);
	wxTimerEvent evt_dummy;
	on_timer(evt_dummy);

	// The background thread needs to queue messages for this dialog
	// and for that it needs a valid pointer to it (mandated by the wxWidgets API).
	// Here we put the pointer under a shared_ptr and protect it by a mutex,
	// so that both threads can access it safely.
	auto dguard = std::make_shared<LifetimeGuard>(this);

	bonjour = std::move(Bonjour("octoprint")
		.set_timeout(10)
		.on_reply([dguard](BonjourReply &&reply) {
			std::lock_guard<std::mutex> lock_guard(dguard->mutex);
			auto dialog = dguard->dialog;
			if (dialog != nullptr) {
				auto evt = new BonjourReplyEvent(EVT_BONJOUR_REPLY, dialog->GetId(), std::move(reply));
				wxQueueEvent(dialog, evt);
			}
		})
		.on_complete([dguard]() {
			std::lock_guard<std::mutex> lock_guard(dguard->mutex);
			auto dialog = dguard->dialog;
			if (dialog != nullptr) {
				// auto evt = new BonjourReplyEvent(EVT_BONJOUR_REPLY, dialog->GetId(), std::move(reply));
				auto evt = new wxCommandEvent(EVT_BONJOUR_COMPLETE, dialog->GetId());
				wxQueueEvent(dialog, evt);
			}
		})
		.lookup()
	);

	bool res = ShowModal() == wxID_OK && list->GetFirstSelected() >= 0;
	{
		// Tell the background thread the dialog is going away...
		std::lock_guard<std::mutex> lock_guard(dguard->mutex);
		dguard->dialog = nullptr;
	}
	return res;
}

wxString BonjourDialog::get_selected() const
{
	auto sel = list->GetFirstSelected();
	return list->GetItemText(sel);
}


// Private

void BonjourDialog::on_reply(BonjourReplyEvent &e)
{
	auto port_suffix = e.reply.port == 80 ? wxString() : wxString::Format(":%d", e.reply.port);
	auto ip = e.reply.ip.to_string();
	auto address = e.reply.path == "/" ?
		wxString::Format("%s%s", ip, port_suffix)
		: wxString::Format("http://%s%s%s", ip, port_suffix, e.reply.path);

	auto item = this->list->GetItemCount();
	this->list->InsertItem(item, std::move(address));
	this->list->SetItem(item, 1, e.reply.hostname);
	this->list->SetItem(item, 2, e.reply.service_name);
	this->list->SetItem(item, 3, e.reply.version);
	this->list->SetItemData(item, item);   // item's data = its original index

	for (int i = 0; i < 4; i++) {
		this->list->SetColumnWidth(i, wxLIST_AUTOSIZE_USEHEADER);
	}

	this->list->SortItems([](wxIntPtr item1, wxIntPtr item2, wxIntPtr self) {
		auto list = reinterpret_cast<wxListView*>(self);

		// Since the items in the list move about during sorting, the original indices are invalidated
		// and so we need to use FindItem() here instead of just using item1/2 as indicies (it's quite silly).
		auto addr1 = list->GetItemText(list->FindItem(-1, item1));
		auto addr2 = list->GetItemText(list->FindItem(-1, item2));
		return addr1.compare(addr2);
	}, reinterpret_cast<wxIntPtr>(this->list));

	auto *parent = this->GetParent();
	if (parent != nullptr) {
		auto size = this->list->GetSize();
		this->list->SetSize(this->list->GetViewRect().GetWidth(), size.GetHeight());
	}

	this->Fit();
}

void BonjourDialog::on_timer(wxTimerEvent &)
{
	const auto search_str = _(L("Searching for devices"));

	if (timer_state > 0) {
		const std::string dots(timer_state, '.');
		label->SetLabel(wxString::Format("%s %s", search_str, dots));
		timer_state = (timer_state) % 3 + 1;
	} else {
		label->SetLabel(wxString::Format("%s: %s", search_str, _(L("Finished."))));
		timer->Stop();
	}
}


}
