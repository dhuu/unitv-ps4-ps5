#include "catalog.h"
#include "jobs.h"

#include <map>
#include <memory>

namespace catalog {

struct Bucket {
    Load state = Load::None;
    std::vector<Stream> items;
};

static std::vector<Category> g_cats[3];
static bool g_catsLoaded = false;
static std::map<std::string, std::shared_ptr<Bucket>> g_buckets;
static int g_generation = 0; // invalida respostas antigas após logout/recarga

static int idx(Kind k) { return k == Kind::Live ? 0 : (k == Kind::Series ? 2 : 1); }

const std::vector<Category>& categories(Kind k) { return g_cats[idx(k)]; }
bool categoriesLoaded() { return g_catsLoaded; }

void loadCategories(std::function<void(bool)> done)
{
    int gen = g_generation;
    struct Three { std::vector<Category> v[3]; };
    auto res = std::make_shared<Three>();
    auto ok = std::make_shared<bool>(false);
    jobs::run(
        [res, ok]() {
            bool a = api::categories(Kind::Live, res->v[0]);
            bool b = api::categories(Kind::Vod, res->v[1]);
            bool c = api::categories(Kind::Series, res->v[2]);
            *ok = a || b || c;
        },
        [res, ok, done, gen]() {
            if (gen != g_generation) return;
            if (*ok) {
                for (int i = 0; i < 3; i++) g_cats[i] = std::move(res->v[i]);
                g_catsLoaded = true;
            }
            if (done) done(*ok);
        });
}

static std::string key(Kind k, const std::string& cat) { return std::to_string(idx(k)) + "|" + cat; }

static void startLoad(Kind k, const std::string& cat, std::shared_ptr<Bucket> b)
{
    b->state = Load::Loading;
    int gen = g_generation;
    auto out = std::make_shared<std::vector<Stream>>();
    auto ok = std::make_shared<bool>(false);
    jobs::run([k, cat, out, ok]() { *ok = api::streams(k, cat, *out); },
              [b, out, ok, gen]() {
                  if (gen != g_generation) return;
                  if (*ok) {
                      b->items = std::move(*out);
                      b->state = Load::Ready;
                  } else {
                      b->state = Load::Failed;
                  }
              });
}

const std::vector<Stream>* streams(Kind k, const std::string& cat, Load* state)
{
    std::string kk = key(k, cat);
    auto it = g_buckets.find(kk);
    if (it == g_buckets.end()) {
        auto b = std::make_shared<Bucket>();
        g_buckets[kk] = b;
        startLoad(k, cat, b);
        if (state) *state = Load::Loading;
        return nullptr;
    }
    if (state) *state = it->second->state;
    return it->second->state == Load::Ready ? &it->second->items : nullptr;
}

void retry(Kind k, const std::string& cat)
{
    auto it = g_buckets.find(key(k, cat));
    if (it != g_buckets.end() && it->second->state == Load::Failed) startLoad(k, cat, it->second);
}

const std::vector<Stream>* all(Kind k, Load* state)
{
    return streams(k, "", state);
}

std::string categoryName(Kind k, const std::string& id)
{
    for (auto& c : g_cats[idx(k)]) if (c.id == id) return c.name;
    return "";
}

void clear()
{
    g_generation++;
    for (auto& c : g_cats) c.clear();
    g_catsLoaded = false;
    g_buckets.clear();
}

} // namespace catalog
