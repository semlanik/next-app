
#include <map>
#include <chrono>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/json.hpp>

#include "nextapp/GrpcServer.h"
#include "nextapp/Server.h"

using namespace std;
using namespace std::literals;
using namespace std::chrono_literals;
using namespace std;
namespace json = boost::json;
namespace asio = boost::asio;

namespace nextapp::grpc {

boost::uuids::uuid newUuid()
{
    static boost::uuids::random_generator uuid_gen_;
    return uuid_gen_();
}

string newUuidStr()
{
    return boost::uuids::to_string(newUuid());
}


namespace {

template <typename T>
concept ProtoMessage = std::is_base_of_v<google::protobuf::Message, T>;

template <ProtoMessage T>
std::string toJson(const T& obj) {
    std::string str;
    auto res = google::protobuf::util::MessageToJsonString(obj, &str);
    if (!res.ok()) {
        LOG_DEBUG << "Failed to convert object to json: "
                  << typeid(T).name() << ": "
                  << res.ToString();
        throw std::runtime_error{"Failed to convert object to json"};
    }
    return str;
}

template <typename T>
concept ProtoStringStringMap = std::is_same_v<std::remove_cv<T>, std::remove_cv<::google::protobuf::Map<std::string, std::string>>>;


template <ProtoStringStringMap T>
string toJson(const T& map) {
    json::object o;

    for(const auto [key, value] : map) {
        o[key] = value;
    }

    return json::serialize(o);
}

std::string toAnsiDate(const nextapp::pb::Date& date) {
    return format("{:0>4d}-{:0>2d}-{:0>2d}", date.year(), date.month() + 1, date.mday());
}

::nextapp::pb::Date toDate(const boost::mysql::date& from) {
    assert(from.valid());
    assert(from.month() > 0);
    ::nextapp::pb::Date date;
    date.set_year(from.year());
    date.set_month(from.month() -1); // Our range is 0 - 11, the db's range is 1 - 12
    date.set_mday(from.day());

    return date;
}

void setError(pb::Status& status, pb::Error err, const std::string& message = {}) {


    status.set_error(err);
    if (message.empty()) {
        status.set_message(pb::Error_Name(err));
    } else {
        status.set_message(message);
    }

    LOG_DEBUG << "Setting error " << status.message() << " on request.";
}

struct ToNode {
    enum Cols {
        ID, USER, NAME, KIND, DESCR, ACTIVE, PARENT, VERSION
    };

    static constexpr string_view selectCols = "id, user, name, kind, descr, active, parent, version";

    static void assign(const boost::mysql::row_view& row, pb::Node& node) {
        node.set_uuid(row.at(ID).as_string());
        node.set_user(row.at(USER).as_string());
        node.set_name(row.at(NAME).as_string());
        node.set_version(row.at(VERSION).as_int64());
        const auto kind = row.at(KIND).as_int64();
        if (pb::Node::Kind_IsValid(kind)) {
            node.set_kind(static_cast<pb::Node::Kind>(kind));
        }
        if (!row.at(DESCR).is_null()) {
            node.set_descr(row.at(DESCR).as_string());
        }
        node.set_active(row.at(ACTIVE).as_int64() != 0);
        if (!row.at(PARENT).is_null()) {
            node.set_parent(row.at(PARENT).as_string());
        }
    }
};

} // anon ns

::grpc::ServerUnaryReactor *
GrpcServer::NextappImpl::GetServerInfo(::grpc::CallbackServerContext *ctx,
                                       const pb::Empty *,
                                       pb::ServerInfo *reply)
{
    assert(ctx);
    assert(reply);

    auto add = [&reply](string key, string value) {
        auto prop = reply->mutable_properties()->Add();
        prop->set_key(key);
        prop->set_value(value);
    };

    add("version", NEXTAPP_VERSION);

    auto* reactor = ctx->DefaultReactor();
    reactor->Finish(::grpc::Status::OK);
    return reactor;
}

::grpc::ServerUnaryReactor *
GrpcServer::NextappImpl::GetDayColorDefinitions(::grpc::CallbackServerContext *ctx,
                                                const pb::Empty *req,
                                                pb::DayColorDefinitions *reply)
{
    auto rval = unaryHandler(ctx, req, reply,
                        [this] (auto *reply) -> boost::asio::awaitable<void> {
        auto res = co_await owner_.server().db().exec(
            "SELECT id, name, color, score FROM day_colors WHERE tenant IS NULL ORDER BY score DESC");

        enum Cols {
            ID, NAME, COLOR, SCORE
        };

        for(const auto row : res.rows()) {
            auto *dc = reply->add_daycolors();
            dc->set_id(row.at(ID).as_string());
            dc->set_color(row.at(COLOR).as_string());
            dc->set_name(row.at(NAME).as_string());
            dc->set_score(static_cast<int32_t>(row.at(SCORE).as_int64()));
        }

        boost::asio::deadline_timer timer{owner_.server().ctx()};
        timer.expires_from_now(boost::posix_time::seconds{2});
        //co_await timer.async_wait(asio::use_awaitable);

        LOG_TRACE_N << "Finish day colors lookup.";
        LOG_TRACE << "Reply is: " << toJson(*reply);
        co_return;
    });

    LOG_TRACE_N << "Leaving the coro do do it's magic...";
    return rval;
}

::grpc::ServerUnaryReactor *
GrpcServer::NextappImpl::GetDay(::grpc::CallbackServerContext *ctx,
                                const pb::Date *req,
                                pb::CompleteDay *reply)
{
    return unaryHandler(ctx, req, reply,
                        [this, req, ctx] (pb::CompleteDay *reply) -> boost::asio::awaitable<void> {

        const auto cuser = owner_.currentUser(ctx);

        auto res = co_await owner_.server().db().exec(
            "SELECT date, user, color, notes, report FROM day WHERE user=? AND date=? ORDER BY date",
            cuser, toAnsiDate(*req));

        enum Cols {
            DATE, USER, COLOR, NOTES, REPORT
        };

        auto* day = reply->mutable_day();
        if (!res.empty() && !res.rows().empty()) {
            const auto& row = res.rows().front();
            const auto date_val = row.at(DATE).as_date();

            *day->mutable_date() = toDate(date_val);
            if (row.at(USER).is_string()) {
                day->set_user(row.at(USER).as_string());
            }
            if (row.at(COLOR).is_string()) {
                day->set_color(row.at(COLOR).as_string());
            }
            if (row.at(NOTES).is_string()) {
                day->set_hasnotes(true);
                reply->set_notes(row.at(NOTES).as_string());
            }
            if (row.at(REPORT).is_string()) {
                day->set_hasreport(true);
                reply->set_report(row.at(REPORT).as_string());
            }
        } else {
            *day->mutable_date() = *req;
            day->set_user(cuser);
        }

        LOG_TRACE << "Finish day lookup: " << toJson(*reply);
        co_return;
    });
}

::grpc::ServerUnaryReactor *GrpcServer::NextappImpl::GetMonth(::grpc::CallbackServerContext *ctx, const pb::MonthReq *req, pb::Month *reply)
{
    return unaryHandler(ctx, req, reply,
                        [this, req, ctx] (pb::Month *reply) -> boost::asio::awaitable<void> {

        auto res = co_await owner_.server().db().exec(
            "SELECT date, user, color, ISNULL(notes), ISNULL(report) FROM day WHERE user=? AND YEAR(date)=? AND MONTH(date)=? ORDER BY date",
            owner_.currentUser(ctx), req->year(), req->month() + 1);

        enum Cols {
            DATE, USER, COLOR, NOTES, REPORT
        };

        reply->set_year(req->year());
        reply->set_month(req->month());

        for(const auto& row : res.rows()) {
            const auto date_val = row.at(DATE).as_date();
            if (date_val.valid()) {
                auto current_day = reply->add_days();
                *current_day->mutable_date() = toDate(date_val);
                current_day->set_user(row.at(USER).as_string());
                if (row.at(COLOR).is_string()) {
                    current_day->set_color(row.at(COLOR).as_string());
                }
                current_day->set_hasnotes(row.at(NOTES).as_int64() != 1);
                current_day->set_hasreport(row.at(REPORT).as_int64() != 1);
            }
        }

        LOG_TRACE_N << "Finish month lookup.";
        LOG_TRACE << "Reply is: " << toJson(*reply);
        co_return;
    });
}

::grpc::ServerUnaryReactor *GrpcServer::NextappImpl::SetColorOnDay(::grpc::CallbackServerContext *ctx, const pb::SetColorReq *req, pb::Status *reply)
{
    return unaryHandler(ctx, req, reply,
        [this, req, ctx] (auto *reply) -> boost::asio::awaitable<void> {

        optional<string> color;
        if (!req->color().empty()) {
            color = req->color();
        }


        co_await owner_.server().db().exec(
            R"(INSERT INTO day (date, user, color) VALUES (?, ?, ?)
                ON DUPLICATE KEY UPDATE color=?)",
            toAnsiDate(req->date()), owner_.currentUser(ctx),
            // insert
            color,
            // update
            color
            );


        LOG_TRACE_N << "Finish updating color for " << toAnsiDate(req->date());

        auto update = make_shared<pb::Update>();
        auto dc = update->mutable_daycolor();
        *dc->mutable_date() = req->date();
        dc->set_user(owner_.currentUser(ctx));
        dc->set_color(req->color());

        owner_.publish(update);
        co_return;
    });
}

::grpc::ServerUnaryReactor *GrpcServer::NextappImpl::SetDay(::grpc::CallbackServerContext *ctx, const pb::CompleteDay *req, pb::Status *reply)
{
    return unaryHandler(ctx, req, reply,
    [this, req, ctx] (auto *reply) -> boost::asio::awaitable<void> {

        optional<string> color;
        if (!req->day().color().empty()) {
                color = req->day().color();
        }

        // We want non-existing entries stored as NULL in the database
        optional<string> notes;
        optional<string> report;

        if (!req->notes().empty()) {
            notes = req->notes();
        }

        if (!req->report().empty()) {
            report = req->report();
        }

        co_await owner_.server().db().exec(
            R"(INSERT INTO day (date, user, color, notes, report) VALUES (?, ?, ?, ?, ?)
                ON DUPLICATE KEY UPDATE color=?, notes=?, report=?)",
            toAnsiDate(req->day().date()), owner_.currentUser(ctx),
            // insert
            color,
            notes,
            report,
            // update
            color,
            notes,
            report
            );

        auto update = make_shared<pb::Update>();
        *update->mutable_day() = *req;

        LOG_DEBUG << "req: " << toJson(*req);
        LOG_DEBUG << "update: " << toJson(*update);

        owner_.publish(update);
        co_return;
    });
}

::grpc::ServerWriteReactor<pb::Update> *GrpcServer::NextappImpl::SubscribeToUpdates(::grpc::CallbackServerContext *context, const pb::UpdatesReq *request)
{
    class ServerWriteReactorImpl
        : public std::enable_shared_from_this<ServerWriteReactorImpl>
        , public Publisher
        , public ::grpc::ServerWriteReactor<pb::Update> {
    public:
        enum class State {
            READY,
            WAITING_ON_WRITE,
            DONE
        };

        ServerWriteReactorImpl(GrpcServer& owner, ::grpc::CallbackServerContext *context)
            : owner_{owner}, context_{context} {
        }

        ~ServerWriteReactorImpl() {
            LOG_DEBUG_N << "Remote client " << uuid() << " is going...";
        }

        void start() {
            // Tell owner about us
            LOG_DEBUG << "Remote client " << context_->peer() << " is subscribing to updates as subscriber " << uuid();
            self_ = shared_from_this();
            owner_.addPublisher(self_);
            reply();
        }

        /*! Callback event when the RPC is completed */
        void OnDone() override {
            {
                scoped_lock lock{mutex_};
                state_ = State::DONE;
            }

            owner_.removePublisher(uuid());
            self_.reset();
        }

        /*! Callback event when a write operation is complete */
        void OnWriteDone(bool ok) override {
            if (!ok) [[unlikely]] {
                LOG_WARN << "The write-operation failed.";

                // We still need to call Finish or the request will remain stuck!
                Finish({::grpc::StatusCode::UNKNOWN, "stream write failed"});
                scoped_lock lock{mutex_};
                state_ = State::DONE;
                return;
            }

            {
                scoped_lock lock{mutex_};
                updates_.pop();
            }

            reply();
        }

        void publish(const std::shared_ptr<pb::Update>& message) override {
            {
                scoped_lock lock{mutex_};
                updates_.emplace(message);
            }

            reply();
        }

    private:
        void reply() {
            scoped_lock lock{mutex_};
            if (state_ != State::READY || updates_.empty()) {
                return;
            }

            StartWrite(updates_.front().get());

            // TODO: Implement finish if the server shuts down.
            //Finish(::grpc::Status::OK);
        }

        GrpcServer& owner_;
        State state_{State::READY};
        std::queue<std::shared_ptr<pb::Update>> updates_;
        std::mutex mutex_;
        std::shared_ptr<ServerWriteReactorImpl> self_;
        ::grpc::CallbackServerContext *context_;
    };

    try {
        auto handler = make_shared<ServerWriteReactorImpl>(owner_, context);
        handler->start();
        return handler.get(); // The object maintains ownership over itself
    } catch (const exception& ex) {
        LOG_ERROR_N << "Caught exception while adding subscriber to update: " << ex.what();
    }

    return {};
}

::grpc::ServerUnaryReactor *GrpcServer::NextappImpl::CreateTenant(::grpc::CallbackServerContext *ctx, const pb::CreateTenantReq *req, pb::Status *reply)
{
    // Do some basic checks before we attempt to create anything...
    if (!req->has_tenant() || req->tenant().name().empty()) {
        setError(*reply, pb::Error::MISSING_TENANT_NAME);
    } else {

        for(const auto& user : req->users()) {
            if (user.email().empty()) {
                setError(*reply, pb::Error::MISSING_USER_EMAIL);
            } else if (user.name().empty()) {
                setError(*reply, pb::Error::MISSING_USER_NAME);
            }
        }
    }

    if (reply->error() != pb::Error::OK) {
        auto* reactor = ctx->DefaultReactor();
        reactor->Finish(::grpc::Status::OK);
        return reactor;
    }

    LOG_DEBUG_N << "Request to create tenant " << req->tenant().name();

    return unaryHandler(ctx, req, reply,
                        [this, req, ctx] (pb::Status *reply) -> boost::asio::awaitable<void> {

        pb::Tenant tenant{req->tenant()};
        if (tenant.uuid().empty()) {
            tenant.set_uuid(newUuidStr());
        }
        if (tenant.properties().empty()) {
            tenant.mutable_properties();
        }

        const auto properties = toJson(*tenant.mutable_properties());
        if (!tenant.has_kind()) {
            tenant.set_kind(pb::Tenant::Tenant::Kind::Tenant_Kind_Guest);
        }

        co_await owner_.server().db().exec(
            "INSERT INTO tenant (id, name, kind, descr, active, properties) VALUES (?, ?, ?, ?, ?, ?)",
                tenant.uuid(),
                tenant.name(),
                pb::Tenant::Kind_Name(tenant.kind()),
                tenant.descr(),
                tenant.active(),
                properties);

        LOG_INFO << "User " << owner_.currentUser(ctx)
                 << " has created tenant name=" << tenant.name() << ", id=" << tenant.uuid()
                 << ", kind=" << pb::Tenant::Kind_Name(tenant.kind());

        // create users
        for(const auto& user_template : req->users()) {
            pb::User user{user_template};

            if (user.uuid().empty()) {
                user.set_uuid(newUuidStr());
            }

            user.set_tenant(tenant.uuid());
            auto kind = user.kind();
            if (!user.has_kind()) {
                user.set_kind(pb::User::Kind::User_Kind_Regular);
            }

            if (!user.has_active()) {
                user.set_active(true);
            }

            auto user_props = toJson(*user.mutable_properties());
            co_await owner_.server().db().exec(
                "INSERT INTO user (id, tenant, name, email, kind, active, descr, properties) VALUES (?,?,?,?,?,?,?,?)",
                    user.uuid(),
                    user.tenant(),
                    user.name(),
                    user.email(),
                    pb::User::Kind_Name(user.kind()),
                    user.active(),
                    user.descr(),
                    user_props);

            LOG_INFO << "User " << owner_.currentUser(ctx)
                     << " has created user name=" << user.name() << ", id=" << user.uuid()
                     << ", kind=" << pb::User::Kind_Name(user.kind())
                     << ", tenant=" << user.tenant();
        }

        // TODO: Publish the new tenant and users

        *reply->mutable_tenant() = tenant;

        co_return;
    });
}

::grpc::ServerUnaryReactor *GrpcServer::NextappImpl::CreateNode(::grpc::CallbackServerContext *ctx, const pb::CreateNodeReq *req, pb::Status *reply)
{
    LOG_DEBUG << "Request to create node " << req->node().uuid() << " for tenant " << owner_.currentTenant(ctx);

    return unaryHandler(ctx, req, reply,
                        [this, req, ctx] (pb::Status *reply) -> boost::asio::awaitable<void> {


        const auto cuser = owner_.currentUser(ctx);
        optional<string> parent = req->node().parent();
        if (parent->empty()) {
            parent.reset();
        } else {
            co_await owner_.validateParent(*parent, cuser);
        }

        auto id = req->node().uuid();
        if (id.empty()) {
            id = newUuidStr();
        }

        bool active = true;
        if (!req->node().has_active()) {
            active = req->node().active();
        }

        enum Cols {
            ID, USER, NAME, KIND, DESCR, ACTIVE, PARENT, VERSION
        };

        const auto res = co_await owner_.server().db().exec(format(
            "INSERT INTO node (id, user, name, kind, descr, active, parent) VALUES (?, ?, ?, ?, ?, ?, ?) "
            "RETURNING {}", ToNode::selectCols),
               id,
               cuser,
               req->node().name(),
               static_cast<int>(req->node().kind()),
               req->node().descr(),
               active,
               parent);

        if (!res.empty()) {
            auto node = reply->mutable_node();
            ToNode::assign(res.rows().front(), *node);
            reply->set_error(pb::Error::OK);
        } else {
            assert(false); // Should get exception on error
        }

        // Notify clients
        auto update = make_shared<pb::Update>();
        auto node = update->mutable_node();
        *node = reply->node();
        update->set_op(pb::Update::Operation::Update_Operation_ADDED);
        owner_.publish(update);

        co_return;
    });
}

::grpc::ServerUnaryReactor *GrpcServer::NextappImpl::UpdateNode(::grpc::CallbackServerContext *ctx, const pb::Node *req, pb::Status *reply)
{
    LOG_DEBUG << "Request to update node " << req->uuid() << " for tenant " << owner_.currentTenant(ctx);

    return unaryHandler(ctx, req, reply,
        [this, req, ctx] (pb::Status *reply) -> boost::asio::awaitable<void> {
            // Get the existing node

        const auto cuser = owner_.currentUser(ctx);

        bool moved = false;
        bool data_changed = false;

        for(auto retry = 0;; ++retry) {

            const pb::Node existing = co_await owner_.fetcNode(req->uuid(), cuser);

            // Check if any data has changed
            data_changed = req->name() != existing.name()
                                || req->active() != existing.active()
                                || req->kind() != existing.kind()
                                || req->descr() != existing.descr();

            // Check if the parent has changed.
            if (req->parent() != existing.parent()) {
                throw db_err{pb::Error::DIFFEREENT_PARENT, "UpdateNode cannot move nodes in the tree"};
            }

            // Update the data, if version is unchanged
            auto res = co_await owner_.server().db().exec(
                "UPDATE node SET name=?, active=?, kind=?, descr=?, version=version+1 WHERE id=? AND user=? AND version=?",
                req->name(),
                req->active(),
                static_cast<int>(req->kind()),
                req->descr(),
                req->uuid(),
                cuser,
                existing.version()
                );

            if (res.affected_rows() > 0) {
                break; // Only succes-path out of the loop
            }

            LOG_DEBUG << "updateNode: Failed to update. Looping for retry.";
            if (retry >= 5) {
                throw db_err(pb::Error::DATABASE_UPDATE_FAILED, "I failed to update, despite retrying");
            }

            boost::asio::steady_timer timer{owner_.server().ctx()};
            timer.expires_from_now(100ms);
            co_await timer.async_wait(boost::asio::use_awaitable);
        }

        // Get the current record
        const pb::Node current = co_await owner_.fetcNode(req->uuid(), cuser);

        // Notify clients about changes

        reply->set_error(pb::Error::OK);
        *reply->mutable_node() = current;

        // Notify clients
        auto update = make_shared<pb::Update>();
        update->set_op(pb::Update::Operation::Update_Operation_UPDATED);
        *update->mutable_node() = current;
        owner_.publish(update);

        co_return;
    });
}

::grpc::ServerUnaryReactor *GrpcServer::NextappImpl::MoveNode(::grpc::CallbackServerContext *ctx, const pb::MoveNodeReq *req, pb::Status *reply)
{
    LOG_DEBUG << "Request to move node " << req->uuid() << " for tenant " << owner_.currentTenant(ctx);

    return unaryHandler(ctx, req, reply,
    [this, req, ctx] (pb::Status *reply) -> boost::asio::awaitable<void> {
        // Get the existing node

        const auto cuser = owner_.currentUser(ctx);

        for(auto retry = 0;; ++retry) {

            const pb::Node existing = co_await owner_.fetcNode(req->uuid(), cuser);

            if (existing.parent() == req->parentuuid()) {
                reply->set_error(pb::Error::NO_CHANGES);
                reply->set_message("The parent has not changed. Ignoring the reqest!");
                co_return;
            }

            if (req->parentuuid() == req->uuid()) {
                reply->set_error(pb::Error::CONSTRAINT_FAILED);
                reply->set_message("A node cannot be its own parent. Ignoring the request!");
                LOG_DEBUG << "A node cannot be its own parent. Ignoring the request for node-id " << req->uuid();
                co_return;
            }

            optional<string> parent;
            if (!req->parentuuid().empty()) {
                co_await owner_.validateParent(req->parentuuid(), cuser);
                parent = req->parentuuid();
            }

            // Update the data, if version is unchanged
            auto res = co_await owner_.server().db().exec(
                "UPDATE node SET parent=?, version=version+1 WHERE id=? AND user=? AND version=?",
                parent,
                req->uuid(),
                cuser,
                existing.version()
                );

            if (res.affected_rows() > 0) {
                break; // Only succes-path out of the loop
            }

            LOG_DEBUG << "updateNode: Failed to update. Looping for retry.";
            if (retry >= 5) {
                throw db_err(pb::Error::DATABASE_UPDATE_FAILED, "I failed to update, despite retrying");
            }

            boost::asio::steady_timer timer{owner_.server().ctx()};
            timer.expires_from_now(100ms);
            co_await timer.async_wait(boost::asio::use_awaitable);
        }

        // Get the current record
        const pb::Node current = co_await owner_.fetcNode(req->uuid(), cuser);
        // Notify clients about changes

        reply->set_error(pb::Error::OK);
        *reply->mutable_node() = current;

        // Notify clients
        auto update = make_shared<pb::Update>();
        update->set_op(pb::Update::Operation::Update_Operation_MOVED);
        *update->mutable_node() = current;
        owner_.publish(update);

        co_return;
    });
}

::grpc::ServerUnaryReactor *GrpcServer::NextappImpl::DeleteNode(::grpc::CallbackServerContext *ctx, const pb::DeleteNodeReq *req, pb::Status *reply)
{
    LOG_DEBUG << "Request to delete node " << req->uuid() << " for tenant " << owner_.currentTenant(ctx);

    return unaryHandler(ctx, req, reply,
    [this, req, ctx] (pb::Status *reply) -> boost::asio::awaitable<void> {
        // Get the existing node

        const auto cuser = owner_.currentUser(ctx);

        const auto node = co_await owner_.fetcNode(req->uuid(), cuser);

        auto res = co_await owner_.server().db().exec(format("DELETE from node where id=? and user=?", ToNode::selectCols),
                                                      req->uuid(), cuser);

        if (!res.has_value() || res.affected_rows() == 0) {
            throw db_err{pb::Error::NOT_FOUND, format("Node {} not found", req->uuid())};
        }

        reply->set_error(pb::Error::OK);
        *reply->mutable_node() = node;

        // Notify clients
        auto update = make_shared<pb::Update>();
        update->set_op(pb::Update::Operation::Update_Operation_DELETED);
        *update->mutable_node() = node;
        owner_.publish(update);

        co_return;
    });
}

::grpc::ServerUnaryReactor *GrpcServer::NextappImpl::GetNodes(::grpc::CallbackServerContext *ctx,
                                                              const pb::GetNodesReq *req,
                                                              pb::NodeTree *reply)
{
    return unaryHandler(ctx, req, reply,
    [this, req, ctx] (pb::NodeTree *reply) -> boost::asio::awaitable<void> {
        const auto cuser = owner_.currentUser(ctx);

        const auto res = co_await owner_.server().db().exec(format(R"(
        WITH RECURSIVE tree AS (
          SELECT * FROM node WHERE user=?
          UNION
          SELECT n.* FROM node AS n, tree AS p
          WHERE n.parent = p.id or n.parent IS NULL
        )
        SELECT {} from tree ORDER BY parent, name)", ToNode::selectCols), cuser);

        std::deque<pb::NodeTreeItem> pending;
        map<string, pb::NodeTreeItem *> known;

        // Root level
        known[""] = reply->mutable_root();

        if (res.has_value()) {
            for(const auto& row : res.rows()) {
                pb::Node n;
                ToNode::assign(row, n);
                const auto parent = n.parent();

                if (auto it = known.find(parent); it != known.end()) {
                    auto child = it->second->add_children();
                    child->mutable_node()->Swap(&n);
                    known[child->node().uuid()] = child;
                } else {
                    // Track it for later
                    const auto id = n.uuid();
                    pending.push_back({});
                    auto child = &pending.back();
                    child->mutable_node()->Swap(&n);
                    known[child->node().uuid()] = child;
                }
            }
        }

        // By now, all the parents are in the known list.
        // We can safely move all the pending items to the child lists of the parents
        for(auto& v : pending) {
            if (auto it = known.find(v.node().parent()); it != known.end()) {
                auto id = v.node().uuid();
                auto& parent = *it->second;
                parent.add_children()->Swap(&v);
                // known lookup must point to the node's new memory location
                assert(parent.children().size() > 0);
                known[id] = &parent.mutable_children()->at(parent.children().size()-1);
            } else {
                assert(false);
            }
        }

        co_return;
    });
}

GrpcServer::GrpcServer(Server &server)
    : server_{server}
{
}

void GrpcServer::start() {
    ::grpc::ServerBuilder builder;

    // Tell gRPC what TCP address/port to listen to and how to handle TLS.
    // grpc::InsecureServerCredentials() will use HTTP 2.0 without encryption.
    builder.AddListeningPort(config().address, ::grpc::InsecureServerCredentials());

    // Feed gRPC our implementation of the RPC's
    service_ = std::make_unique<NextappImpl>(*this);
    builder.RegisterService(service_.get());

    // Finally assemble the server.
    grpc_server_ = builder.BuildAndStart();
    LOG_INFO
        // Fancy way to print the class-name.
        // Useful when I copy/paste this code around ;)
        << boost::typeindex::type_id_runtime(*this).pretty_name()

        // The useful information
        << " listening on " << config().address;
}

void GrpcServer::stop() {
    LOG_INFO << "Shutting down "
             << boost::typeindex::type_id_runtime(*this).pretty_name();
    grpc_server_->Shutdown();
    grpc_server_->Wait();
}

void GrpcServer::addPublisher(const std::shared_ptr<Publisher> &publisher)
{
    LOG_TRACE_N << "Adding publisher " << publisher->uuid();
    scoped_lock lock{mutex_};
    publishers_[publisher->uuid()] = publisher;
}

void GrpcServer::removePublisher(const boost::uuids::uuid &uuid)
{
    LOG_TRACE_N << "Removing publisher " << uuid;
    scoped_lock lock{mutex_};
    publishers_.erase(uuid);
}

void GrpcServer::publish(const std::shared_ptr<pb::Update>& update)
{
    scoped_lock lock{mutex_};

    LOG_DEBUG_N << "Publishing update to " << publishers_.size() << " subscribers, Json: "
                << toJson(*update);

    for(auto& [uuid, weak_pub]: publishers_) {
        if (auto pub = weak_pub.lock()) {
            pub->publish(update);
        } else {
            LOG_WARN_N << "Failed to get a pointer to publisher " << uuid;
        }
    }
}

boost::asio::awaitable<void> GrpcServer::validateParent(const std::string &parentUuid, const std::string &userUuid)
{
    auto res = co_await server().db().exec("SELECT id FROM node where id=? and user=?", parentUuid, userUuid);
    if (!res.has_value()) {
        throw db_err{pb::Error::INVALID_PARENT, "Parent id must exist and be owned by the user"};
    }

    co_return;
}

boost::asio::awaitable<pb::Node> GrpcServer::fetcNode(const std::string &uuid, const std::string &userUuid)
{
    auto res = co_await server().db().exec(format("SELECT {} from node where id=? and user=?", ToNode::selectCols),
                                           uuid, userUuid);
    if (!res.has_value()) {
        throw db_err{pb::Error::NOT_FOUND, format("Node {} not found", uuid)};
    }

    pb::Node rval;
    ToNode::assign(res.rows().front(), rval);
    co_return rval;
}


} // ns
