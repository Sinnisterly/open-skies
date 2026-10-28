#include "core/Features.h"
#include "core/AppLog.h"

namespace os {

Registry& Registry::Get()
{
    static Registry r;
    return r;
}

Feature& Registry::Add(Feature f)
{
    if (f.values.empty())
        for (auto& s : f.sliders) f.values.push_back(s.def);
    m_features.push_back(std::make_unique<Feature>(std::move(f)));
    return *m_features.back();
}

Feature* Registry::Find(const std::string& id)
{
    for (auto& f : m_features)
        if (f->id == id) return f.get();
    return nullptr;
}

std::vector<Feature*> Registry::All()
{
    std::vector<Feature*> out;
    out.reserve(m_features.size());
    for (auto& f : m_features) out.push_back(f.get());
    return out;
}

void Registry::Post(std::function<void()> fn)
{
    std::lock_guard lk(m_mx);
    m_queue.push_back(std::move(fn));
}

void Registry::Pump()
{
    std::vector<std::function<void()>> q;
    {
        std::lock_guard lk(m_mx);
        q.swap(m_queue);
    }
    for (auto& fn : q) fn();
}

void Registry::SetOn(Feature& f, bool on)
{
    if (f.kind != Kind::Toggle || f.on == on) return;
    if (f.avail != Avail::Ready) { f.on = on; return; } // remembered, applied once ready
    bool ok = true;
    try { if (f.onToggle) ok = f.onToggle(on); }
    catch (...) { ok = false; }
    if (ok) f.on = on;
    else log::Warn("Feature", "{} failed to {}", f.id, on ? "enable" : "disable");
}

void Registry::SetSlider(Feature& f, int idx, float v)
{
    if (idx < 0 || idx >= (int)f.values.size()) return;
    f.values[idx] = v;
    if (f.avail == Avail::Ready && f.onSlider) f.onSlider(idx, v);
}

void Registry::Run(Feature& f)
{
    if (f.avail == Avail::Ready && f.onAction) f.onAction();
}

void Registry::DisableAll()
{
    for (auto& f : m_features)
        if (f->kind == Kind::Toggle && f->on) SetOn(*f, false);
    log::Info("Feature", "All features disabled");
}

int Registry::ReadyCount() const
{
    int n = 0;
    for (auto& f : m_features) n += f->avail == Avail::Ready;
    return n;
}

int Registry::TotalCount() const
{
    int n = 0;
    for (auto& f : m_features) n += f->avail != Avail::Planned;
    return n;
}

}
