#include "FileDialog.h"

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

bool ShowOnnxFileDialog(std::string& out_path_utf8) {
    out_path_utf8.clear();

    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        [panel setTitle:@"Select YOLO Pose ONNX Model"];
        [panel setAllowedContentTypes:@[
            [UTType typeWithFilenameExtension:@"onnx"]
        ]];

        if ([panel runModal] == NSModalResponseOK) {
            NSURL* url = [[panel URLs] firstObject];
            if (url) {
                const char* path = [[url path] UTF8String];
                if (path) {
                    out_path_utf8 = path;
                    return true;
                }
            }
        }
    }
    return false;
}

#endif // __APPLE__
