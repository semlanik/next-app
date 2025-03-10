#pragma once

#include <queue>
#include <qqmlregistration.h>

#include <memory>

#include "nextapp_client.grpc.qpb.h"

#include <QObject>
#include "logging.h"

template <typename T, typename... Y>
concept IsValidFunctor = std::invocable<T&, Y...>;


class ServerComm : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QString version
                   READ version
                   NOTIFY versionChanged)

    Q_PROPERTY(QString defaultServerAddress READ getDefaultServerAddress CONSTANT)
public:
    using colors_in_months_t = std::shared_ptr<QList<QUuid>>;

    explicit ServerComm();
    ~ServerComm();

    void start();
    void stop();

    [[nodiscard]] QString version();

    // Called when the servers app settings may have changed
    Q_INVOKABLE void reloadSettings();

    static ServerComm& instance() noexcept {
        assert(instance_);
        return *instance_;
    }

    static const nextapp::pb::ServerInfo& getServerInfo() noexcept {
        return instance().server_info_;
    }

    void getColorsInMonth(unsigned year, unsigned month);

    void setDayColor(int year, int month, int day, QUuid colorUuid);
    void setDay(const nextapp::pb::CompleteDay& day);
    void addNode(const nextapp::pb::Node& node);

    // node.parent cannot be changed. Only data-values can be updated.
    void updateNode(const nextapp::pb::Node& node);

    // Move node to another parent
    void moveNode(const QUuid &uuid, const QUuid &toParentUuid);

    void deleteNode(const QUuid& uuid);

    void getNodeTree();

    void getDayColorDefinitions();

    void fetchDay(int year, int month, int day);

    static QString getDefaultServerAddress() {
        return SERVER_ADDRESS;
    }

    Q_INVOKABLE void createPerson(const nextapp::pb::User &user);

signals:
    void versionChanged();
    void dayColorDefinitionsChanged();
    void errorRecieved(const QString &value);

    // When the server has replied to our request fo the colors for this month
    void monthColorsChanged(unsigned year, unsigned month, colors_in_months_t colors);

    // When an update-message is received from the server regarding a change for a color on a day
    void dayColorChanged(unsigned year, unsigned month, unsigned day, QUuid uuid);

    // When we get the full node-list
    void receivedNodeTree(const nextapp::pb::NodeTree& tree);

    void receivedMonth(const nextapp::pb::Month& month);

    void receivedDay(const nextapp::pb::CompleteDay& day);

    // Triggered on all updates from the server
    void onUpdate(const std::shared_ptr<nextapp::pb::Update>& update);

    void receivedDayColorDefinitions(const nextapp::pb::DayColorDefinitions& defs);

private:
    void errorOccurred(const QGrpcStatus &status);
    void onServerInfo(nextapp::pb::ServerInfo info);
    void onGrpcReady();
    void onUpdateMessage();

    struct GrpcCallOptions {
        bool enable_queue = true;
    };

    template <typename respT, typename callT, typename doneT, typename ...Args>
    void callRpc_(callT&& call, doneT && done, const GrpcCallOptions& opts, Args... args) {

        auto exec = [this, call=std::move(call), done=std::move(done), args...]() {
            auto rpc_method = call(args...);
            rpc_method->subscribe(this, [this, rpc_method, done=std::move(done)] () {
                respT rval = rpc_method-> template read<respT>();
                if constexpr (IsValidFunctor<doneT, respT>) {
                    done(rval);
                } else {
                    // The done functor must either be valid callable functor, or 'false'
                    static_assert(std::is_same_v<doneT, bool>);
                    assert(!done);
                }
            },
            [this](QGrpcStatus status) {
                LOG_ERROR_N << "Comm error: " << status.message();
            });
        };

        if (opts.enable_queue && !grpc_is_ready_) {
            grpc_queue_.emplace(std::move(exec));
            return;
        }
        exec();
    }

    template <typename respT, typename callT, typename doneT, typename ...Args>
        requires IsValidFunctor<doneT, respT>
    void callRpc(callT&& call, doneT && done, const GrpcCallOptions& opts, Args... args) {
        callRpc_<respT>(std::move(call), std::move(done), opts, args...);
    }

    template <typename respT, typename callT, typename doneT, typename ...Args>
        requires IsValidFunctor<doneT, respT>
    void callRpc(callT&& call, doneT && done, Args... args) {
        const GrpcCallOptions opts;
        callRpc_<respT>(std::move(call), std::move(done), opts, args...);
    }

    template <typename respT, typename callT,typename ...Args>
    void callRpc(callT&& call, const GrpcCallOptions& opts, Args... arg) {
        callRpc_<respT>(std::move(call), false, opts, arg...);
    }

    template <typename respT, typename callT,typename ...Args>
    void callRpc(callT&& call, Args... arg) {
        const GrpcCallOptions opts;
        callRpc_<respT>(std::move(call), false, opts, arg...);
    }

    std::unique_ptr<nextapp::pb::Nextapp::Client> client_;
    nextapp::pb::ServerInfo server_info_;
    QString server_version_{"Unknown"};
    std::queue<std::function<void()>> grpc_queue_;
    bool grpc_is_ready_ = false;
    static ServerComm *instance_;
    std::shared_ptr<QGrpcServerStream> updates_;
    QString current_server_address_;
};
