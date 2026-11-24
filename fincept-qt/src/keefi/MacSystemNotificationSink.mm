#import <Foundation/Foundation.h>
#include "keefi/ProductIdentity.h"
#import <UserNotifications/UserNotifications.h>
#include "keefi/SystemNotificationSink_p.h"
#include <QUuid>

namespace keefi::native_detail {
NSString* const Category=@"KEEFI_CHART_ALERT_V1";
NSString* const DisableDrip=@"KEEFI_DISABLE_DRIP";
NSString* const OpenWindow=@"KEEFI_OPEN";
NSString* const PrivatePrefix=@"keefi.g8.private.";
NSString* const PublicPrefix=@"keefi.g8.public.";
struct State {
    std::mutex mutex;
    NativeNoticeCallbacks callbacks;
    std::shared_ptr<NativeNoticeAdmission> admission;
    const QString session=QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool closed=false;
};
NSString* ns(const QString& value){const auto bytes=value.toUtf8();return [[NSString alloc] initWithBytes:bytes.constData() length:bytes.size() encoding:NSUTF8StringEncoding];}
QString qt(NSString* value){return value?QString::fromUtf8(value.UTF8String):QString{};}
template<typename Function> void callbacks(const std::shared_ptr<State>& state,Function action){
    NativeNoticeCallbacks current;
    {std::lock_guard lock(state->mutex);if(state->closed)return;current=state->callbacks;}
    action(current);
}
void purge(UNUserNotificationCenter* center,const QStringList& identifiers){
    if(!center||identifiers.isEmpty())return;
    NSMutableArray<NSString*>* values=[NSMutableArray arrayWithCapacity:identifiers.size()];
    for(const auto& id:identifiers)if(auto* value=ns(id))[values addObject:value];
    if(values.count){[center removePendingNotificationRequestsWithIdentifiers:values];[center removeDeliveredNotificationsWithIdentifiers:values];}
}
void reportSettings(const std::shared_ptr<State>& state,UNNotificationSettings* settings){
    SystemNoticePermission permission=SystemNoticePermission::Unknown;QString status;
    switch(settings.authorizationStatus){
    case UNAuthorizationStatusAuthorized:
        permission=SystemNoticePermission::Granted;
        status=settings.alertSetting==UNNotificationSettingEnabled?QStringLiteral("시스템 알림 허용"):QStringLiteral("시스템 알림 권한 허용 · 배너 표시 꺼짐");break;
    case UNAuthorizationStatusProvisional:
        permission=SystemNoticePermission::Granted;status=QStringLiteral("시스템 알림 조용히 허용 · 배너 없음");break;
    case UNAuthorizationStatusDenied:
        permission=SystemNoticePermission::Denied;status=QStringLiteral("시스템 알림 권한 거부 · macOS 설정에서 변경할 수 있어요");break;
    case UNAuthorizationStatusNotDetermined:
    default:status=QStringLiteral("시스템 알림 권한 미선택 · 알림 설정에서 허용하세요");break;
    }
    callbacks(state,[permission,status](const NativeNoticeCallbacks& cb){if(cb.permission)cb.permission(permission,status);});
}
void refreshSettings(const std::shared_ptr<State>& state,UNUserNotificationCenter* center){
    [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* settings){reportSettings(state,settings);}];
}
bool previousPrivateRequest(UNNotificationRequest* request,const QString& currentSession){
    id value=request.content.userInfo[@"keefi_session"];
    const auto previousSession=[value isKindOfClass:NSString.class]?qt(value):QString{};
    return previousPrivateNoticeSession(qt(request.identifier),previousSession,currentSession);
}
void removePreviousPrivateNotifications(UNUserNotificationCenter* center,const QString& currentSession){
    // Only opaque routing IDs are inspected; no title/body/holdings are read.
    __weak UNUserNotificationCenter* weakCenter=center;
    [center getPendingNotificationRequestsWithCompletionHandler:^(NSArray<UNNotificationRequest*>* requests){
        QStringList ids;for(UNNotificationRequest* request in requests)if(previousPrivateRequest(request,currentSession))ids.append(qt(request.identifier));purge(weakCenter,ids);
    }];
    [center getDeliveredNotificationsWithCompletionHandler:^(NSArray<UNNotification*>* notifications){
        QStringList ids;for(UNNotification* notification in notifications)if(previousPrivateRequest(notification.request,currentSession))ids.append(qt(notification.request.identifier));purge(weakCenter,ids);
    }];
}
bool allowedPresentation(const std::shared_ptr<State>& state,UNNotification* notification){
    UNNotificationContent* content=notification.request.content;
    if(![content.categoryIdentifier isEqualToString:Category])return false;
    NSString* identifier=notification.request.identifier;
    const bool privateId=[identifier hasPrefix:PrivatePrefix],publicId=[identifier hasPrefix:PublicPrefix];
    if(!privateId&&!publicId)return false;
    id privateValue=content.userInfo[@"keefi_private"],epochValue=content.userInfo[@"keefi_epoch"];
    if(![privateValue isKindOfClass:NSNumber.class]||![epochValue isKindOfClass:NSNumber.class]||[privateValue boolValue]!=privateId)return false;
    std::shared_ptr<NativeNoticeAdmission> admission;
    {std::lock_guard lock(state->mutex);if(state->closed)return false;admission=state->admission;}
    return admission&&admission->permits({qt(identifier),{},{},privateId,[epochValue unsignedLongLongValue]});
}
} // namespace keefi::native_detail

@interface KEEFIG8NotificationDelegate:NSObject<UNUserNotificationCenterDelegate> {
@public
    std::shared_ptr<keefi::native_detail::State> state;
}
@end
@implementation KEEFIG8NotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter*)center willPresentNotification:(UNNotification*)notification withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
    Q_UNUSED(center);
    UNNotificationPresentationOptions options=UNNotificationPresentationOptionNone;
    if(keefi::native_detail::allowedPresentation(state,notification)){
        options=UNNotificationPresentationOptionBanner|UNNotificationPresentationOptionList;
        if(notification.request.content.sound)options|=UNNotificationPresentationOptionSound;
    }
    completionHandler(options);
}
- (void)userNotificationCenter:(UNUserNotificationCenter*)center didReceiveNotificationResponse:(UNNotificationResponse*)response withCompletionHandler:(void (^)(void))completionHandler {
    Q_UNUSED(center);
    // These two actions change global UI/settings only. They never inspect or
    // restore a private payload, and remain useful after a vault lock.
    if([response.notification.request.content.categoryIdentifier isEqualToString:keefi::native_detail::Category]){
        const auto action=response.actionIdentifier;
        if([action isEqualToString:keefi::native_detail::DisableDrip])
            keefi::native_detail::callbacks(state,[](const keefi::NativeNoticeCallbacks& cb){if(cb.action)cb.action(keefi::SystemNoticeAction::DisableDrip);});
        else if([action isEqualToString:keefi::native_detail::OpenWindow]||[action isEqualToString:UNNotificationDefaultActionIdentifier])
            keefi::native_detail::callbacks(state,[](const keefi::NativeNoticeCallbacks& cb){if(cb.action)cb.action(keefi::SystemNoticeAction::OpenWindow);});
    }
    completionHandler();
}
@end

namespace keefi {
namespace {
class MacNotificationPort final:public NativeNotificationPort {
  public:
    MacNotificationPort():state_(std::make_shared<native_detail::State>()){}
    ~MacNotificationPort() override {stop();}
    void bind(NativeNoticeCallbacks callbacks,std::shared_ptr<NativeNoticeAdmission> admission) override {
        {std::lock_guard lock(state_->mutex);state_->callbacks=std::move(callbacks);state_->admission=std::move(admission);}
        // Unbundled test/CLI executables do not initialize the OS notification
        // center. Unit tests inject FakeNativePort instead of this factory.
        NSString* bundle=NSBundle.mainBundle.bundleIdentifier;
        if(![bundle isEqualToString:@"xyz.beamgrove.keefi.local"]&&![bundle isEqualToString:@"xyz.beamgrove.keefi.dev"]){unavailable();return;}
        @try {
            center_=[UNUserNotificationCenter currentNotificationCenter];
            if(!center_){unavailable();return;}
            delegate_=[KEEFIG8NotificationDelegate new];delegate_->state=state_;center_.delegate=delegate_;
            UNNotificationAction* disable=[UNNotificationAction actionWithIdentifier:native_detail::DisableDrip title:@"드립 모드 끄기" options:UNNotificationActionOptionNone];
            UNNotificationAction* open=[UNNotificationAction actionWithIdentifier:native_detail::OpenWindow title:@"창 열기" options:UNNotificationActionOptionForeground];
            UNNotificationCategory* category=[UNNotificationCategory categoryWithIdentifier:native_detail::Category actions:@[disable,open] intentIdentifiers:@[] options:UNNotificationCategoryOptionNone];
            [center_ setNotificationCategories:[NSSet setWithObject:category]];
            native_detail::removePreviousPrivateNotifications(center_,state_->session);
        } @catch(NSException* exception) {Q_UNUSED(exception);center_=nil;delegate_=nil;unavailable();}
    }
    void refreshPermission() override {
        if(stopped_)return;if(!center_){unavailable();return;}native_detail::refreshSettings(state_,center_);
    }
    void requestAuthorization() override {
        if(stopped_)return;if(!center_){unavailable();return;}
        const auto state=state_;UNUserNotificationCenter* center=center_;
        __weak UNUserNotificationCenter* weakCenter=center;
        [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert|UNAuthorizationOptionSound) completionHandler:^(BOOL granted,NSError* error){
            Q_UNUSED(granted);
            if(error)native_detail::callbacks(state,[](const NativeNoticeCallbacks& cb){if(cb.permission)cb.permission(SystemNoticePermission::Unknown,QStringLiteral("시스템 알림 권한 요청 실패"));});
            else if(UNUserNotificationCenter* current=weakCenter)native_detail::refreshSettings(state,current);
        }];
    }
    void add(const SystemNotice& notice) override {
        const auto state=state_;std::shared_ptr<NativeNoticeAdmission> admission;
        {std::lock_guard lock(state->mutex);admission=state->admission;}
        NativeNoticeReceipt receipt{notice.identifier,notice.privateEpoch,notice.privateData,false};
        if(stopped_||!center_||!admission||!admission->permits(notice)){
            native_detail::callbacks(state,[receipt](const NativeNoticeCallbacks& cb){if(cb.submitted)cb.submitted(receipt);});return;
        }
        UNMutableNotificationContent* content=[UNMutableNotificationContent new];
        content.title=native_detail::ns(notice.title);content.body=native_detail::ns(notice.body);content.categoryIdentifier=native_detail::Category;
        if(notice.playSound)content.sound=UNNotificationSound.defaultSound;
        content.userInfo=@{@"keefi_private":@(notice.privateData),@"keefi_epoch":@(notice.privateEpoch),@"keefi_session":native_detail::ns(state->session)};
        UNNotificationRequest* request=[UNNotificationRequest requestWithIdentifier:native_detail::ns(notice.identifier) content:content trigger:nil];
        UNUserNotificationCenter* center=center_;
        __weak UNUserNotificationCenter* weakCenter=center;
        // Capture routing metadata and shared admission, never the content/body
        // or this port. This callback can finish after stop/destruction.
        [center addNotificationRequest:request withCompletionHandler:^(NSError* error){
            UNUserNotificationCenter* completionCenter=weakCenter;
            NativeNoticeReceipt completed=receipt;completed.accepted=error==nil;
            completed=reconcileNativeNoticeReceipt(completed,admission,[completionCenter](const QStringList& ids){native_detail::purge(completionCenter,ids);});
            native_detail::callbacks(state,[completed](const NativeNoticeCallbacks& cb){if(cb.submitted)cb.submitted(completed);});
        }];
    }
    void remove(const QStringList& identifiers) override {native_detail::purge(center_,identifiers);}
    void stop() override {
        if(stopped_)return;stopped_=true;
        {std::lock_guard lock(state_->mutex);state_->closed=true;if(state_->admission)state_->admission->stop();state_->callbacks={};}
        if(center_.delegate==delegate_)center_.delegate=nil;
        delegate_=nil;center_=nil;
    }
  private:
    void unavailable(){native_detail::callbacks(state_,[](const NativeNoticeCallbacks& cb){if(cb.permission)cb.permission(SystemNoticePermission::Unavailable,QStringLiteral("macOS 시스템 알림 연결 없음 · %1 앱 번들에서 사용할 수 있어요").arg(productName()));});}
    std::shared_ptr<native_detail::State> state_;
    UNUserNotificationCenter* __strong center_=nil;
    KEEFIG8NotificationDelegate* __strong delegate_=nil;
    bool stopped_=false;
};
}
std::unique_ptr<NativeNotificationPort> makeMacSystemNotificationPort(){return std::make_unique<MacNotificationPort>();}
} // namespace keefi
