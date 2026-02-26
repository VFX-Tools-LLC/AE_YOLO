// AE_YOLO_Panel.jsx — ScriptUI companion panel for YOLO Pose plugin
// Creates 17 Null layers expression-linked to YOLO Pose keypoint parameters

(function(thisObj) {
    var PLUGIN_NAME = "YOLO Pose";
    var PLUGIN_MATCH = "YOLO Pose Estimation";

    var KEYPOINT_NAMES = [
        "Nose",   "LEye",   "REye",   "LEar",   "REar",
        "LShldr", "RShldr", "LElbow", "RElbow", "LWrist",
        "RWrist", "LHip",   "RHip",   "LKnee",  "RKnee",
        "LAnkle", "RAnkle"
    ];

    // COCO skeleton connections for visualization
    var SKELETON_PAIRS = [
        [0, 1],  [0, 2],   // Nose → eyes
        [1, 3],  [2, 4],   // Eyes → ears
        [5, 6],             // Shoulders
        [5, 7],  [7, 9],   // Left arm
        [6, 8],  [8, 10],  // Right arm
        [5, 11], [6, 12],  // Torso
        [11, 12],           // Hips
        [11, 13], [13, 15], // Left leg
        [12, 14], [14, 16]  // Right leg
    ];

    function buildPanel(panel) {
        panel.orientation = "column";
        panel.alignment = ["fill", "top"];

        var grp = panel.add("group");
        grp.alignment = ["fill", "top"];
        grp.orientation = "column";

        var btnCreate = grp.add("button", undefined, "Create Nulls from YOLO Pose");
        btnCreate.alignment = ["fill", "top"];
        btnCreate.helpTip = "Select a layer with the YOLO Pose effect, then click to create 17 expression-linked Null layers";

        var btnDelete = grp.add("button", undefined, "Remove YOLO Nulls");
        btnDelete.alignment = ["fill", "top"];
        btnDelete.helpTip = "Remove all YOLO Pose Null layers from the active comp";

        var statusText = grp.add("statictext", undefined, "Select a layer with YOLO Pose effect");
        statusText.alignment = ["fill", "top"];

        btnCreate.onClick = function() {
            app.beginUndoGroup("Create YOLO Pose Nulls");
            try {
                createNulls(statusText);
            } catch (e) {
                alert("Error: " + e.toString());
            }
            app.endUndoGroup();
        };

        btnDelete.onClick = function() {
            app.beginUndoGroup("Remove YOLO Pose Nulls");
            try {
                removeNulls(statusText);
            } catch (e) {
                alert("Error: " + e.toString());
            }
            app.endUndoGroup();
        };
    }

    function findYoloPoseEffect(layer) {
        for (var i = 1; i <= layer.property("ADBE Effect Parade").numProperties; i++) {
            var fx = layer.property("ADBE Effect Parade").property(i);
            if (fx.matchName === PLUGIN_MATCH || fx.name === PLUGIN_NAME) {
                return fx;
            }
        }
        return null;
    }

    function createNulls(statusText) {
        var comp = app.project.activeItem;
        if (!comp || !(comp instanceof CompItem)) {
            statusText.text = "No active composition";
            return;
        }

        var selLayers = comp.selectedLayers;
        if (selLayers.length === 0) {
            statusText.text = "Select a layer with YOLO Pose";
            return;
        }

        var srcLayer = selLayers[0];
        var fx = findYoloPoseEffect(srcLayer);
        if (!fx) {
            statusText.text = "No YOLO Pose effect on selected layer";
            return;
        }

        var srcName = srcLayer.name;
        var fxName = fx.name;

        // Create 17 null layers
        var nulls = [];
        for (var k = 0; k < KEYPOINT_NAMES.length; k++) {
            var nullLayer = comp.layers.addNull();
            nullLayer.name = "YP_" + KEYPOINT_NAMES[k];
            nullLayer.label = 9; // Green label
            nullLayer.inPoint = srcLayer.inPoint;
            nullLayer.outPoint = srcLayer.outPoint;

            // Set position expression (point param returns [x, y] directly)
            var posExpr =
                'var src = thisComp.layer("' + srcName + '");\n' +
                'var fx = src.effect("' + fxName + '");\n' +
                'src.toComp(fx("' + KEYPOINT_NAMES[k] + '"));';
            nullLayer.property("ADBE Transform Group").property("ADBE Position").expression = posExpr;

            // Set opacity from confidence
            var opExpr =
                'var src = thisComp.layer("' + srcName + '");\n' +
                'var fx = src.effect("' + fxName + '");\n' +
                'fx("' + KEYPOINT_NAMES[k] + '_Conf") * 100;';
            nullLayer.property("ADBE Transform Group").property("ADBE Opacity").expression = opExpr;

            nulls.push(nullLayer);
        }

        statusText.text = "Created " + nulls.length + " Null layers";
    }

    function removeNulls(statusText) {
        var comp = app.project.activeItem;
        if (!comp || !(comp instanceof CompItem)) {
            statusText.text = "No active composition";
            return;
        }

        var removed = 0;
        // Remove in reverse order to avoid index shifting
        for (var i = comp.numLayers; i >= 1; i--) {
            var layer = comp.layer(i);
            if (layer.name.indexOf("YP_") === 0 && layer.nullLayer) {
                layer.remove();
                removed++;
            }
        }

        statusText.text = removed > 0 ? "Removed " + removed + " Null layers" : "No YOLO Nulls found";
    }

    // Build as dockable panel or dialog
    if (thisObj instanceof Panel) {
        buildPanel(thisObj);
    } else {
        var win = new Window("palette", "YOLO Pose", undefined, { resizeable: true });
        buildPanel(win);
        win.center();
        win.show();
    }

})(this);
