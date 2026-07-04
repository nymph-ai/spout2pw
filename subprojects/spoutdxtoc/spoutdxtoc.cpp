#include "spoutdxtoc.h"
#include "actaeid_pipewire_video_contract.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

/* Include SpoutDX and ignore its headers warnings */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"

#include "SpoutDirectX.h"
#include "SpoutFrameCount.h"
#include "SpoutSenderNames.h"
#include "SpoutUtils.h"

#pragma GCC diagnostic pop

struct SpoutDXToCSenderNames {
    spoutSenderNames sendernames;
};

struct SpoutDXToCReceiver {
    std::string sendername;
    uint32_t lastShareHandle;
    uint32_t lastAdapterId;
    ID3D11Texture2D *sharedTexture;
    ID3D11Texture2D *receiveTexture;
    HANDLE receiveShareHandle;

    spoutSenderNames sendernames;
    spoutFrameCount frame;
    spoutDirectX dx;

    CRITICAL_SECTION cs;
    bool texture_locked;
    bool dx_open;
};

static void ReleaseReceiveTexture(SPOUTDXTOC_RECEIVER *self) {
    if (self->receiveTexture) {
        self->receiveTexture->Release();
        self->receiveTexture = nullptr;
    }
    self->receiveShareHandle = nullptr;
}

static std::vector<std::string> collect_allowed_sender_names(
    spoutSenderNames &sendernames) {
    std::vector<std::string> senderlist;

    const int nSenders = sendernames.GetSenderCount();
    if (nSenders <= 0)
        return senderlist;

    char sendername[256]{};
    for (int i = 0; i < nSenders; i++) {
        if (!sendernames.GetSender(i, sendername))
            continue;

        std::string candidate(sendername);
        if (actaeid::pipewire_video_contract::AllowsSourceName(candidate))
            senderlist.push_back(candidate);
    }

    return senderlist;
}

static std::string join_sender_names(const std::vector<std::string> &senderlist) {
    std::string names;
    for (size_t i = 0; i < senderlist.size(); i++) {
        if (i != 0)
            names += ", ";
        names += senderlist[i];
    }
    return names;
}

static std::string configured_contract_sender_name() {
    {
        std::ifstream file("C:\\spout2pw-selected-sender.txt");
        if (file.good()) {
            std::string value;
            std::getline(file, value);
            while (!value.empty() &&
                   (value.back() == '\r' || value.back() == '\n' ||
                    value.back() == ' ' || value.back() == '\t')) {
                value.pop_back();
            }
            if (!value.empty())
                return value;
        }
    }

    const char *env_name =
        actaeid::pipewire_video_contract::SourceSelectedSenderEnvVar();
    DWORD required = GetEnvironmentVariableA(env_name, nullptr, 0);
    if (required > 1) {
        std::string value(required - 1, '\0');
        if (GetEnvironmentVariableA(env_name, &value[0], required) ==
            required - 1) {
            return value;
        }
    }

    const char *raw =
        std::getenv(env_name);
    if (!raw || !raw[0])
        return {};

    return std::string(raw);
}

static std::string active_allowed_sender_name(
    spoutSenderNames &sendernames,
    const std::vector<std::string> &senderlist) {
    char active_sender[256]{};
    if (!sendernames.GetActiveSender(active_sender, sizeof(active_sender)))
        return {};

    const std::string active(active_sender);
    const auto it = std::find(senderlist.begin(), senderlist.end(), active);
    if (it == senderlist.end())
        return {};

    return *it;
}

static bool allows_contract_receiver_name(const std::string &sender_name) {
    if (!actaeid::pipewire_video_contract::AllowsSourceName(sender_name))
        return false;

    const auto configured_sender = configured_contract_sender_name();
    if (configured_sender.empty())
        return true;

    return sender_name == configured_sender;
}

static std::vector<std::string> select_contract_sender_names(
    spoutSenderNames &sendernames) {
    const auto senderlist = collect_allowed_sender_names(sendernames);
    const auto configured_sender = configured_contract_sender_name();
    if (!configured_sender.empty()) {
        const auto it =
            std::find(senderlist.begin(), senderlist.end(), configured_sender);
        if (it != senderlist.end())
            return {*it};
    } else if (senderlist.size() ==
               actaeid::pipewire_video_contract::Contract::kSourceCount) {
        return senderlist;
    } else {
        const auto active_sender = active_allowed_sender_name(sendernames, senderlist);
        if (!active_sender.empty())
            return {active_sender};
    }

    const auto names = join_sender_names(senderlist);

    if (!configured_sender.empty()) {
        if (senderlist.empty()) {
            static bool logged_waiting = false;
            if (!logged_waiting) {
                SpoutLogNotice(
                    "Waiting for configured Spout sender %s=%s within family "
                    "prefix '%s'.",
                    actaeid::pipewire_video_contract::SourceSelectedSenderEnvVar(),
                    configured_sender.c_str(),
                    actaeid::pipewire_video_contract::SourceSenderPrefix());
                logged_waiting = true;
            }
            return {};
        }
        SpoutLogError(
            "%s Contract violation for %s: expected selected sender %s=%s "
            "within family prefix '%s', found %zu matching senders [%s].",
            actaeid::pipewire_video_contract::Warning(),
            actaeid::pipewire_video_contract::SourceFamilyName(),
            actaeid::pipewire_video_contract::SourceSelectedSenderEnvVar(),
            configured_sender.c_str(),
            actaeid::pipewire_video_contract::SourceSenderPrefix(),
            senderlist.size(), names.empty() ? "(none)" : names.c_str());
    } else {
        SpoutLogError(
            "%s Contract violation for %s: no unique sender could be selected "
            "from family prefix '%s'. Found %zu matching senders [%s]. Set %s "
            "to one exact sender name to resolve the ambiguity.",
            actaeid::pipewire_video_contract::Warning(),
            actaeid::pipewire_video_contract::SourceFamilyName(),
            actaeid::pipewire_video_contract::SourceSenderPrefix(),
            senderlist.size(), names.empty() ? "(none)" : names.c_str(),
            actaeid::pipewire_video_contract::SourceSelectedSenderEnvVar());
    }
    return {};
}

SPOUTDXTOC_SENDERNAMES *__stdcall SpoutDXToCNewSenderNames(void) {
    spoututils::EnableSpoutLogFile("C:\\spoutlog.txt");
    SpoutLogNotice("%s", actaeid::pipewire_video_contract::Warning());
    const auto configured_sender = configured_contract_sender_name();
    if (!configured_sender.empty()) {
        SpoutLogNotice("Configured contract sender: %s=%s",
                       actaeid::pipewire_video_contract::SourceSelectedSenderEnvVar(),
                       configured_sender.c_str());
    }
    SPOUTDXTOC_SENDERNAMES *p = new SpoutDXToCSenderNames();
    return p;
}

void __stdcall SpoutDXToCFreeSenderNames(SPOUTDXTOC_SENDERNAMES *self) {
    assert(self != NULL);

    delete self;
}

int __stdcall SpoutDXToCGetSenderCount(SPOUTDXTOC_SENDERNAMES *self) {
    assert(self != NULL);

    return static_cast<int>(select_contract_sender_names(self->sendernames).size());
}

#define NAME_MAX_SIZE 256

bool __stdcall SpoutDXToCGetSender(SPOUTDXTOC_SENDERNAMES *self, int64_t index,
                                   char **sendername) {
    assert(self != NULL);
    assert(sendername != NULL && *sendername == NULL);

    auto senderlist = select_contract_sender_names(self->sendernames);
    if (index < 0 || static_cast<size_t>(index) >= senderlist.size())
        return false;

    *sendername = (char *)calloc(1, NAME_MAX_SIZE * sizeof(char));
    strncpy(*sendername, senderlist[index].c_str(), NAME_MAX_SIZE - 1);
    return true;
}

static void vec_to_null_term_clist(std::vector<std::string> &vector,
                                   char ***list) {
    char **name;

    *list = (char **)calloc(vector.size() + 1, sizeof(char *));

    name = *list;
    for (std::string &s : vector) {
        *name = strdup(s.c_str());
        name++;
    }

    name = *list;
    assert(name[vector.size()] == NULL);
}

char **__stdcall SpoutDXToCGetSenderListSimple(SPOUTDXTOC_SENDERNAMES *self,
                                               uint32_t *ret_count) {
    std::vector<std::string> senderlist;
    char **list = NULL;

    assert(self != NULL);
    senderlist = select_contract_sender_names(self->sendernames);

    vec_to_null_term_clist(senderlist, &list);

    if (ret_count != NULL)
        *ret_count = senderlist.size();

    return list;
}

void __stdcall SpoutDXToCNamelistClear(SPOUTDXTOC_NAMELIST *namelist) {
    assert(namelist != NULL);

    if (namelist->list == NULL)
        return;

    for (uint32_t i = 0; namelist->list[i] != NULL; i++)
        free(namelist->list[i]);

    free(namelist->list);
    namelist->list = NULL;
}

bool __stdcall SpoutDXToCGetSenderList(SPOUTDXTOC_SENDERNAMES *self,
                                       SPOUTDXTOC_NAMELIST *old_list,
                                       SPOUTDXTOC_NAMELIST *ret_senders,
                                       SPOUTDXTOC_NAMELIST *ret_added,
                                       SPOUTDXTOC_NAMELIST *ret_removed) {
    std::vector<std::string> senderlist, list, removed;

    assert(self != NULL);

    if (ret_senders == NULL) {
        assert(ret_added != NULL && ret_added->list == NULL);
        assert(ret_removed != NULL && ret_removed->list == NULL);
    } else {
        assert(ret_senders->list == NULL);
        assert(ret_added == NULL || ret_added->list == NULL);
        assert(ret_removed == NULL || ret_removed->list == NULL);
    }

    list = select_contract_sender_names(self->sendernames);
    senderlist = list;

    for (size_t i = 0; i < old_list->count; i++) {
        std::string sender(old_list->list[i]);
        auto it = std::find(list.begin(), list.end(), sender);

        if (it != list.end())
            list.erase(it);
        else
            removed.push_back(sender);
    }

    if (list.empty() && removed.empty())
        return false;

    if (ret_senders != NULL) {
        vec_to_null_term_clist(senderlist, &ret_senders->list);
        ret_senders->count = senderlist.size();
    }

    if (ret_added != NULL) {
        vec_to_null_term_clist(list, &ret_added->list);
        ret_added->count = list.size();
    }

    if (ret_removed != NULL) {
        vec_to_null_term_clist(removed, &ret_removed->list);
        ret_removed->count = removed.size();
    }

    return true;
}

SPOUTDXTOC_RECEIVER *__stdcall SpoutDXToCNewReceiver(const char *SenderName) {
    if (!SenderName || !allows_contract_receiver_name(SenderName)) {
        const auto configured_sender = configured_contract_sender_name();
        if (!configured_sender.empty()) {
            SpoutLogError(
                "%s Refusing sender '%s' because %s=%s requires one exact "
                "sender selection.",
                actaeid::pipewire_video_contract::Warning(),
                SenderName ? SenderName : "(null)",
                actaeid::pipewire_video_contract::SourceSelectedSenderEnvVar(),
                configured_sender.c_str());
        } else {
            SpoutLogError("%s Refusing sender '%s' outside source family %s.",
                          actaeid::pipewire_video_contract::Warning(),
                          SenderName ? SenderName : "(null)",
                          actaeid::pipewire_video_contract::SourceFamilyName());
        }
        return nullptr;
    }

    SPOUTDXTOC_RECEIVER *p = new SpoutDXToCReceiver();

    InitializeCriticalSection(&p->cs);

    p->sendername = std::string(SenderName);
    p->frame.CreateAccessMutex(SenderName);
    p->frame.EnableFrameCount(SenderName);
    return p;
}

void __stdcall SpoutDXToCFreeReceiver(SPOUTDXTOC_RECEIVER *self) {
    assert(self != NULL);

    self->frame.CleanupFrameCount();
    self->frame.CloseAccessMutex();

    if (self->sharedTexture) {
        self->sharedTexture->Release();
        self->sharedTexture = nullptr;
    }
    ReleaseReceiveTexture(self);

    if (self->dx_open)
        self->dx.CloseDirectX11();

    DeleteCriticalSection(&self->cs);

    delete self;
}

bool __stdcall SpoutDXToCIsConnected(SPOUTDXTOC_RECEIVER *self) {
    assert(self != NULL);

    SharedTextureInfo info;
    if (!self->sendernames.getSharedInfo(self->sendername.c_str(), &info))
        return false;

    if (info.width == 0 || info.height == 0 || info.shareHandle == 0)
        return false;

    return true;
}

static bool InitDXTexture(SPOUTDXTOC_RECEIVER *self, uint32_t shareHandle) {
    IDXGIAdapter *pAdapter = nullptr;

    SpoutLogNotice("InitDXTexture %x", shareHandle);

    if (self->sharedTexture) {
        self->sharedTexture->Release();
        self->sharedTexture = nullptr;
    }
    ReleaseReceiveTexture(self);

    if (!shareHandle)
        return false;

    if (self->dx_open) {
        SpoutLogNotice(
            "Importing texture 0x%lx into existing DX adapter (index=%d)",
            shareHandle, self->lastAdapterId);
        // Try to open the share handle with the same device
        if (self->dx.OpenDX11shareHandle(self->dx.GetDX11Device(),
                                         &self->sharedTexture,
                                         LongToHandle((long)shareHandle)))
            return true;

        SpoutLogNotice("Import failed");
        return false;
    }

    // First time
    SpoutLogNotice("Importing texture 0x%lx, trying all adapters", shareHandle);

    const int nAdapters = self->dx.GetNumAdapters();
    for (int i = 0; i < nAdapters; i++) {
        SpoutLogNotice("Trying adapter %d", i);
        if (!self->dx.SetAdapter(i))
            continue;

        // Set the adapter pointer for CreateDX11device to use temporarily
        self->dx.SetAdapterPointer(pAdapter);
        if (!self->dx.OpenDirectX11(nullptr))
            continue;

        // Try to open the share handle with the device created from the adapter
        if (self->dx.OpenDX11shareHandle(self->dx.GetDX11Device(),
                                         &self->sharedTexture,
                                         LongToHandle((long)shareHandle))) {
            self->lastAdapterId = i;
            self->dx_open = true;
            SpoutLogNotice("Texture import succeeded");
            return true;
        }

        self->dx.CloseDirectX11();
    }

    SpoutLogError("All adapters failed to import the texture");

    return false;
}

bool __stdcall SpoutDXToCGetSenderInfo(SPOUTDXTOC_RECEIVER *self,
                                       SPOUTDXTOC_SENDERINFO *info) {
    assert(self != NULL);
    assert(info != NULL);

    SharedTextureInfo sinfo;
    if (!self->sendernames.getSharedInfo(self->sendername.c_str(), &sinfo))
        return false;

    info->shareHandle = (HANDLE)(LongToHandle((long)sinfo.shareHandle));
    info->width = sinfo.width;
    info->height = sinfo.height;
    info->format = sinfo.format;
    info->usage = sinfo.usage;
    info->changed = false;

    if (self->lastShareHandle != sinfo.shareHandle) {
        // Just free the existing texture, defer creating the new one to
        // SpoutDXToCUpdateDXTexture() to work around a race
        EnterCriticalSection(&self->cs);
        self->texture_locked = false;
        InitDXTexture(self, 0);
        LeaveCriticalSection(&self->cs);
        self->lastShareHandle = 0;
        info->changed = true;
    }

    return true;
}

bool SpoutDXToCUpdateDXTexture(SPOUTDXTOC_RECEIVER *self,
                               SPOUTDXTOC_SENDERINFO *info) {

    EnterCriticalSection(&self->cs);

    self->lastShareHandle = 0;
    self->texture_locked = false;
    bool success = InitDXTexture(self, HandleToLong(info->shareHandle));
    if (success) {
        success = self->dx.CreateSharedDX11Texture(
            self->dx.GetDX11Device(), info->width, info->height,
            (DXGI_FORMAT)info->format, &self->receiveTexture,
            self->receiveShareHandle, false, false);
        if (success) {
            SpoutLogNotice(
                "Receiver copy texture created [0x%0lX] for sender [0x%0lX]",
                HandleToLong(self->receiveShareHandle),
                HandleToLong(info->shareHandle));
        }
    }

    LeaveCriticalSection(&self->cs);

    if (!success)
        return false;

    self->lastShareHandle = HandleToLong(info->shareHandle);
    info->adapterId = self->lastAdapterId;
    info->shareHandle = self->receiveShareHandle;

    return true;
}

bool __stdcall SpoutDXToCCheckTextureAccess(SPOUTDXTOC_RECEIVER *self) {
    assert(self != NULL);
    bool ret = true;

    EnterCriticalSection(&self->cs);

    if (self->sharedTexture) {
        self->texture_locked = ret =
            self->frame.CheckTextureAccess(self->sharedTexture);
        if (ret && self->receiveTexture) {
            ID3D11DeviceContext *ctx = self->dx.GetDX11Context();
            if (ctx) {
                ctx->CopyResource(self->receiveTexture, self->sharedTexture);
                self->dx.FlushWait();
            } else {
                ret = false;
            }
        }
    }

    LeaveCriticalSection(&self->cs);
    return ret;
}

bool __stdcall SpoutDXToCAllowTextureAccess(SPOUTDXTOC_RECEIVER *self) {
    assert(self != NULL);
    bool ret = true;

    EnterCriticalSection(&self->cs);

    if (self->sharedTexture && self->texture_locked)
        ret = self->frame.AllowTextureAccess(self->sharedTexture);

    LeaveCriticalSection(&self->cs);
    return ret;
}

bool __stdcall SpoutDXToCGetFrameCount(SPOUTDXTOC_RECEIVER *self,
                                       uint64_t *framecount) {
    assert(self != NULL);

    bool ret = self->frame.GetNewFrame();

    if (framecount)
        *framecount = self->frame.GetSenderFrame();

    return ret;
}
