/*
  ZSoda Depth Extract Panel (ExtendScript / ScriptUI)

  Usage:
  1) Load this script in After Effects (File > Scripts > Run Script File...)
     or place in ScriptUI Panels and relaunch AE.
  2) Select a layer that has the ZSoda effect applied.
  3) Click "Extract Depth Map" to freeze and refresh the baked depth.
*/

(function zsodaDepthExtractPanel(thisObj) {
  function buildUi(host) {
    var win = (host instanceof Panel) ? host : new Window("palette", "ZSoda Extract", undefined, { resizeable: true });
    win.orientation = "column";
    win.alignChildren = ["fill", "top"];

    var statusText = win.add("statictext", undefined, "Ready");
    statusText.characters = 38;

    var extractBtn = win.add("button", undefined, "Extract Depth Map");
    var unfreezeBtn = win.add("button", undefined, "Disable Freeze");

    function findZsodaEffect(layer) {
      if (!layer || !layer.property) {
        return null;
      }
      var parade = layer.property("ADBE Effect Parade");
      if (!parade) {
        return null;
      }
      var i;
      for (i = 1; i <= parade.numProperties; i += 1) {
        var fx = parade.property(i);
        if (!fx) {
          continue;
        }
        var fxName = (fx.name || "").toLowerCase();
        var fxMatch = (fx.matchName || "").toLowerCase();
        if (fxName.indexOf("zsoda") >= 0 || fxName.indexOf("z-soda") >= 0 ||
            fxMatch.indexOf("zsoda") >= 0 || fxMatch.indexOf("z-soda") >= 0) {
          return fx;
        }
      }
      return null;
    }

    function resolveActiveEffect() {
      var comp = app.project && app.project.activeItem;
      if (!(comp && comp instanceof CompItem)) {
        alert("활성 컴포지션을 선택해주세요.");
        return null;
      }
      if (!comp.selectedLayers || comp.selectedLayers.length < 1) {
        alert("ZSoda가 적용된 레이어를 선택해주세요.");
        return null;
      }

      var layer = comp.selectedLayers[0];
      var fx = findZsodaEffect(layer);
      if (!fx) {
        alert("선택한 레이어에서 ZSoda 이펙트를 찾을 수 없습니다.");
        return null;
      }
      return fx;
    }

    function resolveControl(fx, propName) {
      var prop = fx.property(propName);
      if (!prop) {
        alert("ZSoda 파라미터를 찾을 수 없습니다: " + propName);
        return null;
      }
      return prop;
    }

    extractBtn.onClick = function() {
      var fx = resolveActiveEffect();
      if (!fx) {
        statusText.text = "No ZSoda effect selected";
        return;
      }

      var freezeProp = resolveControl(fx, "Freeze Depth");
      if (!freezeProp) {
        return;
      }
      var extractBtnProp = fx.property("Extract Depth Map");
      var tokenProp = fx.property("Extract Token");

      statusText.text = "Extract requested...";
      app.beginUndoGroup("ZSoda Extract Depth");
      try {
        freezeProp.setValue(true);
        if (extractBtnProp) {
          // Button param path (preferred): triggers USER_CHANGED_PARAM in plugin.
          try {
            extractBtnProp.setValue(1);
          } catch (btnErr) {
            if (tokenProp) {
              var currentTokenBtnFail = Math.floor(tokenProp.value);
              if (!isFinite(currentTokenBtnFail) || currentTokenBtnFail < 0) {
                currentTokenBtnFail = 0;
              }
              tokenProp.setValue((currentTokenBtnFail + 1) % 65536);
            } else {
              throw btnErr;
            }
          }
        } else if (tokenProp) {
          // Backward-compatible fallback for older token-based builds.
          var currentToken = Math.floor(tokenProp.value);
          if (!isFinite(currentToken) || currentToken < 0) {
            currentToken = 0;
          }
          tokenProp.setValue((currentToken + 1) % 65536);
        } else {
          throw new Error("Extract control not found (button/token)");
        }
        statusText.text = "Freeze ON, extraction requested";
      } catch (err) {
        statusText.text = "Extract failed";
        alert("Extract Depth Map 실패: " + err.toString());
      } finally {
        app.endUndoGroup();
      }
    };

    unfreezeBtn.onClick = function() {
      var fx = resolveActiveEffect();
      if (!fx) {
        statusText.text = "No ZSoda effect selected";
        return;
      }

      var freezeProp = resolveControl(fx, "Freeze Depth");
      if (!freezeProp) {
        return;
      }

      app.beginUndoGroup("ZSoda Disable Freeze");
      try {
        freezeProp.setValue(false);
        statusText.text = "Freeze OFF";
      } catch (err) {
        statusText.text = "Disable freeze failed";
        alert("Freeze 해제 실패: " + err.toString());
      } finally {
        app.endUndoGroup();
      }
    };

    win.layout.layout(true);
    win.layout.resize();
    return win;
  }

  var ui = buildUi(thisObj);
  if (ui instanceof Window) {
    ui.center();
    ui.show();
  }
})(this);
