#pragma once
#import <Foundation/Foundation.h>
/* Both report families take this lease before scheduling CloudKit operations. */
BOOL host_report_delivery_acquire(void);
void host_report_delivery_release(void);
void host_report_delivery_submit(NSString *path, NSString *asset, NSDictionary *metadata, NSString *hash, dispatch_queue_t callback_queue, void (^completion)(BOOL, NSError *));
/* Called on the performance worker when gameplay or suspension begins. */
void host_report_delivery_cancel_performance(void);
void host_report_delivery_submit_shader(NSString *path, NSString *asset, NSDictionary *metadata, NSString *hash, dispatch_queue_t callback_queue, void (^completion)(BOOL, NSError *));
void host_report_delivery_cancel_shader(void);
