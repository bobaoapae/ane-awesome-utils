// End-to-end test for the native .aneprof profiler capture subsystem.
//
// Covers multiple scenarios in a single run:
//
//   Scenario A:  Start → 120 frames (~4.8 s @ 25 fps) with sprite churn,
//                allocations, inline markers (battle.start / battle.end),
//                and a span metric → Stop.
//                Output: test_capture_A.aneprof
//
//   Scenario B:  Second Start→Stop in the same process lifetime, exercising
//                the "one game can record several battles" use case.
//                Output: test_capture_B.aneprof
//
//   Scenario C:  Long-ish capture (240 frames, ~9.6 s) with timing,
//                memory hooks and periodic snapshots enabled.
//                Output: test_capture_C.aneprof
//
//   Scenario E:  Hidden listener leak: short-lived views are removed from the
//                display list but remain retained by a strong listener on a
//                long-lived dispatcher.
//                Output: test_capture_E.aneprof
//
//   Scenario T:  Timer leak: short-lived views are retained by running timers
//                whose listeners are never removed.
//                Output: test_capture_T.aneprof
//
//   Scenario M:  Closure leak: callbacks stored in a static queue capture
//                removed views and payloads.
//                Output: test_capture_M.aneprof
//
//   Scenario S:  Static display cache leak: removed display objects with
//                BitmapData payloads stay in a long-lived cache.
//                Output: test_capture_S.aneprof
//
//   Scenario L:  Memory capture with intentional retained ByteArrays so the
//                analyzer can distinguish a leak-like run from Scenario C.
//                Output: test_capture_L.aneprof
//
//   Scenario R:  Real AS3 edge hook smoke/stress: display-list add/remove and
//                EventDispatcher listener add/remove with some edges kept live
//                until stop.
//                Output: test_capture_R.aneprof
//
//   Scenario D:  Start + kill — on a run with ANE_TEST_KILL=1 we call Start,
//                run a couple of frames, then let the harness kill us.
//                The .aneprof on disk should still have a valid header even
//                if the footer didn't get written.
//                Output: test_capture_D.aneprof   (partial)
//
// At the end we write a single test_result.json summarising every scenario.

package {
    import flash.desktop.NativeApplication;
    import flash.display.BitmapData;
    import flash.display.Shape;
    import flash.display.Sprite;
    import flash.events.Event;
    import flash.events.EventDispatcher;
    import flash.events.TimerEvent;
    import flash.filesystem.File;
    import flash.filesystem.FileMode;
    import flash.filesystem.FileStream;
    import flash.geom.Point;
    import flash.utils.ByteArray;
    import flash.utils.Dictionary;
    import flash.utils.Timer;
    import flash.utils.setTimeout;
    import flash.utils.getTimer;

    [SWF(frameRate="25", backgroundColor="#112233", width="640", height="480")]
    public class TestProfilerApp extends Sprite {

        private var util:AneAwesomeUtils;
        private var storage:File;
        private var results:Array;
        private var scenarioIndex:int = 0;
        private var scenarios:Array;
        private var killOnScenarioD:Boolean = false;

        // Scenario state while a capture is running.
        private var active:Object;  // {name, outPath, targetFrames, frameCount, lastMarkerSec, churnPool, allocScratch, startTs}
        private static var retainedLeaks:Array = [];
        private static var listenerLeakBus:EventDispatcher = new EventDispatcher();
        private static var closureLeakCallbacks:Array = [];
        private static var staticDisplayCache:Dictionary = new Dictionary();

        public function TestProfilerApp() {
            log("[test] boot");

            util = AneAwesomeUtils.instance;
            if (!util.initialize()) {
                log("[test] ANE initialize() failed");
                exitWith(10);
                return;
            }

            storage = File.applicationStorageDirectory;
            storage.resolvePath("").createDirectory();

            // Clean prior outputs.
            for each (var n:String in ["A", "B", "C", "D", "E", "T", "M", "S", "L", "R"]) {
                removeIfPresent(storage.resolvePath("test_capture_" + n + ".aneprof"));
            }
            removeIfPresent(storage.resolvePath("test_result.json"));

            // Read the `kill` flag from env — when set, scenario D won't
            // call Stop; the harness will terminate us mid-capture.
            killOnScenarioD = File.applicationStorageDirectory
                .resolvePath("ANE_TEST_KILL").exists;

            results = [];
            if (killOnScenarioD) {
                // Kill-test run: single long-running scenario that never calls
                // Stop. The harness terminates us while we're in the middle
                // of Scenario D, then inspects whatever made it to disk.
                scenarios = [
                    { name:"D", label:"kill-mid-capture",    frames:2000,
                      timing:true, memory:false, snapshots:true,
                      doNotStop:true }
                ];
            } else {
                scenarios = [
                    { name:"A", label:"timing-only", frames:120,
                      timing:true, memory:false, snapshots:true },
                    { name:"B", label:"second-run-after-stop", frames:80,
                      timing:true, memory:false, snapshots:true },
                    { name:"C", label:"timing-memory-snapshots", frames:240,
                      timing:true, memory:true, snapshots:true, snapshotIntervalMs:1000 },
                    { name:"R", label:"real-edge-hooks", frames:80,
                      timing:true, memory:true, snapshots:true, snapshotIntervalMs:1000,
                      realEdgeHooks:true },
                    { name:"E", label:"hidden-listener-leak", frames:180,
                      timing:true, memory:true, snapshots:true, snapshotIntervalMs:1000,
                      listenerLeak:true },
                    { name:"T", label:"timer-closure-leak", frames:120,
                      timing:true, memory:true, snapshots:true, snapshotIntervalMs:1000,
                      timerLeak:true },
                    { name:"M", label:"closure-capture-leak", frames:120,
                      timing:true, memory:true, snapshots:true, snapshotIntervalMs:1000,
                      closureLeak:true },
                    { name:"S", label:"static-display-cache-leak", frames:120,
                      timing:true, memory:true, snapshots:true, snapshotIntervalMs:1000,
                      staticCacheLeak:true },
                    { name:"L", label:"intentional-memory-retention", frames:240,
                      timing:true, memory:true, snapshots:true, snapshotIntervalMs:1000,
                      leak:true }
                ];
            }

            setTimeout(runNext, 1000);
        }

        private function runNext():void {
            if (scenarioIndex >= scenarios.length) {
                finalizeAndExit();
                return;
            }

            var sc:Object = scenarios[scenarioIndex];
            active = {
                name:          sc.name,
                label:         sc.label,
                outPath:       storage.resolvePath("test_capture_" + sc.name + ".aneprof").nativePath,
                targetFrames:  int(sc.frames),
                doNotStop:     Boolean(sc.doNotStop),
                leak:          Boolean(sc.leak),
                syntheticBase:  16777216 + (scenarioIndex * 1048576),
                listenerLeak:   Boolean(sc.listenerLeak),
                timerLeak:      Boolean(sc.timerLeak),
                closureLeak:    Boolean(sc.closureLeak),
                staticCacheLeak:Boolean(sc.staticCacheLeak),
                realEdgeHooks:  Boolean(sc.realEdgeHooks),
                listenerLeakCreated: 0,
                timerLeakCreated: 0,
                closureLeakCreated: 0,
                staticCacheCreated: 0,
                realEdgeCreated: 0,
                realEdgeRemoved: 0,
                realEdgeRoot:   null,
                realEdgeBus:    null,
                realEdgeLive:   [],
                nativeGcRequested: false,
                frameCount:    0,
                lastMarkerSec: -1,
                churnPool:     new Vector.<Sprite>(),
                allocScratch:  new Vector.<ByteArray>()
            };
            log("[test] scenario " + sc.name + " (" + sc.label + "): start, frames=" + sc.frames);

            var options:Object = {
                timing:           Boolean(sc.timing),
                memory:           Boolean(sc.memory),
                snapshots:        Boolean(sc.snapshots),
                snapshotIntervalMs: sc.snapshotIntervalMs !== undefined ? uint(sc.snapshotIntervalMs) : 0,
                metadata:         {
                    scenario: sc.name,
                    label: sc.label,
                    leak: Boolean(sc.leak) || Boolean(sc.listenerLeak) ||
                          Boolean(sc.timerLeak) || Boolean(sc.closureLeak) ||
                          Boolean(sc.staticCacheLeak),
                    listenerLeak: Boolean(sc.listenerLeak),
                    timerLeak: Boolean(sc.timerLeak),
                    closureLeak: Boolean(sc.closureLeak),
                    staticCacheLeak: Boolean(sc.staticCacheLeak),
                    realEdgeHooks: Boolean(sc.realEdgeHooks)
                }
            };
            var ok:Boolean = util.profilerStart(active.outPath, options);
            log("[test] profilerStart(" + sc.name + ") = " + ok);
            if (!ok) {
                captureFailedResult(sc.name, "start-returned-false");
                scenarioIndex++;
                setTimeout(runNext, 500);
                return;
            }

            active.startTs = getTimer();
            // Markers that bracket a "battle" in the .aneprof stream.
            util.profilerMarker("battle.start", scenarioIndex + 1);
            addEventListener(Event.ENTER_FRAME, onEnterFrame);
        }

        private function onEnterFrame(e:Event):void {
            ++active.frameCount;

            doSpriteChurn();
            doAllocationWork();
            doHiddenListenerLeakWork();
            doTimerClosureLeakWork();
            doClosureCaptureLeakWork();
            doStaticDisplayCacheLeakWork();
            doRealEdgeHookWork();
            doSyntheticProfilerRecords();
            doComputeWork();

            var nowSec:int = int((getTimer() - active.startTs) / 1000);
            if (nowSec > active.lastMarkerSec) {
                active.lastMarkerSec = nowSec;
                util.profilerMarker("battle.tick", nowSec);
            }

            if (active.frameCount >= active.targetFrames) {
                removeEventListener(Event.ENTER_FRAME, onEnterFrame);
                if (active.doNotStop) {
                    // Scenario D: don't stop; let the harness kill us.
                    log("[test] scenario " + active.name + ": reached frames, waiting for kill");
                    return;
                }
                setTimeout(finishScenario, 50);
            }
        }

        private function finishScenario():void {
            util.profilerMarker("battle.end", scenarioIndex + 1);
            if (active.listenerLeak) {
                util.profilerMarker("listener.leak.created", active.listenerLeakCreated);
            }
            if (active.timerLeak) {
                util.profilerMarker("timer.leak.created", active.timerLeakCreated);
            }
            if (active.closureLeak) {
                util.profilerMarker("closure.leak.created", active.closureLeakCreated);
            }
            if (active.staticCacheLeak) {
                util.profilerMarker("static.cache.leak.created", active.staticCacheCreated);
            }
            if (active.realEdgeHooks) {
                util.profilerMarker("real.edge.created", active.realEdgeCreated);
                util.profilerMarker("real.edge.removed", active.realEdgeRemoved);
            }
            util.profilerSnapshot("pre-release");

            if (active.leak) {
                retainedLeaks.push(active.churnPool);
            } else {
                // Drain the sprite pool so the runtime sees .displayobject.remove
                // events in the final span.
                while (active.churnPool.length > 0) {
                    var sp:Sprite = active.churnPool.shift();
                    if (sp && sp.parent) sp.parent.removeChild(sp);
                }
            }
            if (active.leak) {
                retainedLeaks.push(active.allocScratch);
            } else {
                active.allocScratch.length = 0;
            }

            active.nativeGcRequested = util.profilerRequestGc();
            util.profilerMarker("gc.native.request", {
                scenario: active.name,
                ok: active.nativeGcRequested
            });
            setTimeout(stopAfterNativeGcSnapshot, 250);
        }

        private function stopAfterNativeGcSnapshot():void {
            util.profilerSnapshot("post-native-gc-pre-stop");

            var pre:Object = util.profilerGetStatus();
            var stopOk:Boolean = util.profilerStop();

            setTimeout(function():void {
                var post:Object = util.profilerGetStatus();
                var f:File = new File(active.outPath);
                var fileOk:Boolean = f.exists && f.size > 64;

                var rec:Object = {
                    scenario:        active.name,
                    label:           active.label,
                    targetFrames:    active.targetFrames,
                    framesRan:       active.frameCount,
                    profilerStopOk:  stopOk,
                    fileExists:      f.exists,
                    fileSize:        f.exists ? f.size : 0,
                    fileOk:          fileOk,
                    preStop:         pre,
                    postStop:        post,
                    outputPath:      active.outPath,
                    nativeGcRequested: active.nativeGcRequested,
                    listenerLeakCreated: active.listenerLeakCreated,
                    timerLeakCreated: active.timerLeakCreated,
                    closureLeakCreated: active.closureLeakCreated,
                    staticCacheCreated: active.staticCacheCreated,
                    realEdgeCreated: active.realEdgeCreated,
                    realEdgeRemoved: active.realEdgeRemoved,
                    markers:         ["battle.start", "battle.tick*", "battle.end"]
                };
                log("[test] scenario " + active.name + " done: " + JSON.stringify(rec.postStop));
                results.push(rec);

                cleanupRealEdgeHookWork();
                scenarioIndex++;
                setTimeout(runNext, 500);
            }, 500);
        }

        private function captureFailedResult(name:String, why:String):void {
            results.push({
                scenario:  name,
                failed:    true,
                reason:    why,
                status:    util.profilerGetStatus()
            });
        }

        private function finalizeAndExit():void {
            var anyFail:Boolean = false;
            for each (var r:Object in results) {
                if (r.failed || !r.fileOk || !r.profilerStopOk) { anyFail = true; break; }
            }
            var finalResult:Object = {
                scenarios: results,
                allOk:     !anyFail
            };
            writeResult(finalResult);
            exitWith(anyFail ? 20 : 0);
        }

        // -------- workload ---------------------------------------------------

        private function doSpriteChurn():void {
            for (var i:int = 0; i < 6; i++) {
                var s:Sprite = new Sprite();
                s.x = Math.random() * 600;
                s.y = Math.random() * 400;
                s.graphics.beginFill(0xAA3377, 0.6);
                s.graphics.drawCircle(0, 0, 4 + Math.random() * 6);
                s.graphics.endFill();
                var sh:Shape = new Shape();
                sh.graphics.lineStyle(1, 0x225599);
                sh.graphics.lineTo(10, 10);
                s.addChild(sh);
                addChild(s);
                active.churnPool.push(s);
            }
            while (active.churnPool.length > 40) {
                var old:Sprite = active.churnPool.shift();
                if (old && old.parent) old.parent.removeChild(old);
            }
        }

        private function doAllocationWork():void {
            var arr:Array = [];
            for (var i:int = 0; i < 250; i++) {
                arr.push({k: "key" + i, v: i * 1.5, flag: (i & 1) == 0});
            }
            var dict:Dictionary = new Dictionary();
            for (var j:int = 0; j < 50; j++) {
                dict["item" + j] = new Point(j, j * 2);
            }
            var ba:ByteArray = new ByteArray();
            for (var k:int = 0; k < 512; k++) ba.writeByte(k & 0xFF);
            active.allocScratch.push(ba);
            while (active.allocScratch.length > 16) {
                active.allocScratch.shift();
            }
            if (active.leak) {
                retainedLeaks.push(arr);
                retainedLeaks.push(dict);
                retainedLeaks.push(ba);
                var leak:ByteArray = new ByteArray();
                leak.length = 32768;
                leak.position = leak.length - 1;
                leak.writeByte(active.frameCount & 0xFF);
                retainedLeaks.push(leak);
            }
        }

        private function doHiddenListenerLeakWork():void {
            if (!active.listenerLeak) return;
            for (var i:int = 0; i < 3; i++) {
                var view:HiddenListenerLeak = createHiddenListenerLeak(active.frameCount * 10 + i);
                addChild(view);
                view.renderOnce();
                removeChild(view);
                view.disposeVisualOnly();
                active.listenerLeakCreated++;
            }
            if ((active.frameCount & 7) == 0) {
                listenerLeakBus.dispatchEvent(new Event("modelTick"));
            }
        }

        private function createHiddenListenerLeak(id:int):HiddenListenerLeak {
            return new HiddenListenerLeak(listenerLeakBus, id);
        }

        private function doTimerClosureLeakWork():void {
            if (!active.timerLeak) return;
            for (var i:int = 0; i < 2; i++) {
                var view:TimerClosureLeak = new TimerClosureLeak(active.frameCount * 10 + i);
                addChild(view);
                view.renderOnce();
                removeChild(view);
                view.disposeVisualOnly();
                active.timerLeakCreated++;
            }
        }

        private function doClosureCaptureLeakWork():void {
            if (!active.closureLeak) return;
            for (var i:int = 0; i < 4; i++) {
                var view:ClosureCaptureLeak = new ClosureCaptureLeak(active.frameCount * 10 + i);
                addChild(view);
                view.renderOnce();
                removeChild(view);
                closureLeakCallbacks.push(makeClosureLeakCallback(view));
                active.closureLeakCreated++;
            }
        }

        private function makeClosureLeakCallback(view:ClosureCaptureLeak):Function {
            return function():int {
                return view.touch();
            };
        }

        private function doStaticDisplayCacheLeakWork():void {
            if (!active.staticCacheLeak) return;
            for (var i:int = 0; i < 3; i++) {
                var view:StaticDisplayCacheLeak = new StaticDisplayCacheLeak(active.frameCount * 10 + i);
                addChild(view);
                view.renderOnce();
                removeChild(view);
                staticDisplayCache["view-" + active.frameCount + "-" + i] = view;
                active.staticCacheCreated++;
            }
        }

        private function doRealEdgeHookWork():void {
            if (!active.realEdgeHooks) return;
            if (active.realEdgeRoot == null) {
                active.realEdgeRoot = new Sprite();
                active.realEdgeRoot.name = "realEdgeRoot";
                addChild(active.realEdgeRoot);
                active.realEdgeBus = new EventDispatcher();
            }

            for (var i:int = 0; i < 4; i++) {
                var view:RealEdgeProbeView = new RealEdgeProbeView(active.frameCount * 10 + i);
                view.renderOnce();
                active.realEdgeRoot.addChild(view);
                active.realEdgeCreated++;

                active.realEdgeBus.addEventListener("edgeTick", view.onEdgeTick);
                active.realEdgeCreated++;

                if ((i & 1) == 0) {
                    active.realEdgeBus.removeEventListener("edgeTick", view.onEdgeTick);
                    active.realEdgeRemoved++;
                    if (view.parent) view.parent.removeChild(view);
                    active.realEdgeRemoved++;
                    view.disposeVisualOnly();
                } else {
                    active.realEdgeLive.push(view);
                }
            }

            active.realEdgeBus.dispatchEvent(new Event("edgeTick"));
            while (active.realEdgeLive.length > 120) {
                var old:RealEdgeProbeView = active.realEdgeLive.shift();
                active.realEdgeBus.removeEventListener("edgeTick", old.onEdgeTick);
                active.realEdgeRemoved++;
                if (old.parent) old.parent.removeChild(old);
                active.realEdgeRemoved++;
                old.disposeVisualOnly();
            }
        }

        private function cleanupRealEdgeHookWork():void {
            if (!active || !active.realEdgeHooks) return;
            if (active.realEdgeBus && active.realEdgeLive) {
                for each (var view:RealEdgeProbeView in active.realEdgeLive) {
                    active.realEdgeBus.removeEventListener("edgeTick", view.onEdgeTick);
                    view.disposeVisualOnly();
                }
                active.realEdgeLive.length = 0;
            }
            if (active.realEdgeRoot && active.realEdgeRoot.parent) {
                active.realEdgeRoot.parent.removeChild(active.realEdgeRoot);
            }
            active.realEdgeRoot = null;
            active.realEdgeBus = null;
        }

        private function doSyntheticProfilerRecords():void {
            if (!active) return;
            var i:int;
            var ptr:Number;
            if (active.name == "C") {
                for (i = 0; i < 2; i++) {
                    ptr = Number(active.syntheticBase + active.frameCount * 32 + i);
                    util.profilerRecordAllocForTest(ptr, 2048);
                    util.profilerRecordFreeForTest(ptr);
                }
            }
            if (active.leak) {
                for (i = 0; i < 8; i++) {
                    ptr = Number(active.syntheticBase + active.frameCount * 32 + i);
                    util.profilerRecordAllocForTest(ptr, 8192);
                }
            }
        }

        private function doComputeWork():void {
            outerMath(0);
        }

        private function outerMath(seed:Number):Number {
            var s:Number = seed;
            for (var i:int = 0; i < 80; i++) s = innerMathA(s, i);
            return s;
        }
        private function innerMathA(v:Number, i:int):Number {
            return innerMathB(v + Math.sin(i * 0.1), i);
        }
        private function innerMathB(v:Number, i:int):Number {
            return (v * 1.000001) + Math.cos(i * 0.07);
        }

        // -------- helpers ----------------------------------------------------

        private function writeResult(result:Object):void {
            try {
                var target:File = storage.resolvePath("test_result.json");
                var fs:FileStream = new FileStream();
                fs.open(target, FileMode.WRITE);
                fs.writeUTFBytes(JSON.stringify(result, null, 2));
                fs.close();
                log("[test] wrote " + target.nativePath);
            } catch (e:Error) {
                log("[test] writeResult failed: " + e);
            }
        }

        private function removeIfPresent(f:File):void {
            try { if (f.exists) f.deleteFile(); } catch (e:Error) {}
        }

        private function log(msg:String):void { trace(msg); }

        private function exitWith(code:int):void {
            setTimeout(function():void {
                NativeApplication.nativeApplication.exit(code);
            }, 100);
        }
    }

}

import flash.display.Sprite;
import flash.display.BitmapData;
import flash.events.Event;
import flash.events.EventDispatcher;
import flash.events.TimerEvent;
import flash.geom.Point;
import flash.utils.ByteArray;
import flash.utils.Timer;

internal class HiddenListenerLeak extends Sprite {
    private var bus:EventDispatcher;
    private var payload:ByteArray;
    private var points:Array;
    private var id:int;
    private var ticks:int = 0;

    public function HiddenListenerLeak(bus:EventDispatcher, id:int) {
        this.bus = bus;
        this.id = id;
        payload = new ByteArray();
        payload.length = 16384;
        payload.position = payload.length - 1;
        payload.writeByte(id & 0xff);

        points = [];
        for (var i:int = 0; i < 16; i++) {
            points.push(new Point(id + i, id - i));
        }

        this.bus.addEventListener("modelTick", onModelTick);
    }

    public function renderOnce():void {
        graphics.beginFill(0x339966, 0.35);
        graphics.drawRect(0, 0, 8 + (id % 5), 8 + (id % 7));
        graphics.endFill();
    }

    public function disposeVisualOnly():void {
        graphics.clear();
        if (parent) parent.removeChild(this);
        // Intentional leak for the E2E: the strong listener above is not
        // removed, so the long-lived dispatcher keeps this view and payload.
    }

    private function onModelTick(e:Event):void {
        ticks++;
        if (payload.length > 0) {
            payload.position = 0;
            payload.writeByte((id + ticks) & 0xff);
        }
    }
}

internal class TimerClosureLeak extends Sprite {
    private var timer:Timer;
    private var payload:ByteArray;
    private var points:Array;
    private var id:int;
    private var ticks:int = 0;

    public function TimerClosureLeak(id:int) {
        this.id = id;
        payload = new ByteArray();
        payload.length = 12288;
        payload.position = payload.length - 1;
        payload.writeByte(id & 0xff);
        points = [];
        for (var i:int = 0; i < 12; i++) {
            points.push(new Point(id + i, id * 2 - i));
        }

        var self:TimerClosureLeak = this;
        timer = new Timer(40);
        timer.addEventListener(TimerEvent.TIMER, function(e:TimerEvent):void {
            self.onTimerTick();
        });
        timer.start();
    }

    public function renderOnce():void {
        graphics.beginFill(0x8844cc, 0.35);
        graphics.drawCircle(0, 0, 8 + (id % 4));
        graphics.endFill();
    }

    public function disposeVisualOnly():void {
        graphics.clear();
        if (parent) parent.removeChild(this);
        // Intentional leak: timer keeps running and its closure captures this.
    }

    private function onTimerTick():void {
        ticks++;
        if (payload.length > 0) {
            payload.position = 0;
            payload.writeByte((id + ticks) & 0xff);
        }
    }
}

internal class ClosureCaptureLeak extends Sprite {
    private var payload:ByteArray;
    private var points:Array;
    private var id:int;

    public function ClosureCaptureLeak(id:int) {
        this.id = id;
        payload = new ByteArray();
        payload.length = 8192;
        payload.position = payload.length - 1;
        payload.writeByte(id & 0xff);
        points = [];
        for (var i:int = 0; i < 8; i++) {
            points.push(new Point(id - i, id + i));
        }
    }

    public function renderOnce():void {
        graphics.beginFill(0xcc8844, 0.35);
        graphics.drawRect(0, 0, 9 + (id % 3), 9 + (id % 5));
        graphics.endFill();
    }

    public function touch():int {
        return payload.length + points.length + id;
    }
}

internal class StaticDisplayCacheLeak extends Sprite {
    private var bitmap:BitmapData;
    private var payload:ByteArray;
    private var id:int;

    public function StaticDisplayCacheLeak(id:int) {
        this.id = id;
        bitmap = new BitmapData(96, 96, false, 0x224466 + (id & 0xff));
        payload = new ByteArray();
        payload.length = 24576;
        payload.position = payload.length - 1;
        payload.writeByte(id & 0xff);
    }

    public function renderOnce():void {
        graphics.beginBitmapFill(bitmap);
        graphics.drawRect(0, 0, 24, 24);
        graphics.endFill();
    }
}

internal class RealEdgeProbeView extends Sprite {
    private var payload:ByteArray;
    private var id:int;
    private var ticks:int = 0;

    public function RealEdgeProbeView(id:int) {
        this.id = id;
        payload = new ByteArray();
        payload.length = 4096;
        payload.position = payload.length - 1;
        payload.writeByte(id & 0xff);
    }

    public function renderOnce():void {
        graphics.beginFill(0x22aacc, 0.35);
        graphics.drawRect(0, 0, 10 + (id % 4), 10 + (id % 6));
        graphics.endFill();
    }

    public function onEdgeTick(e:Event):void {
        ticks++;
        if (payload.length > 0) {
            payload.position = 0;
            payload.writeByte((id + ticks) & 0xff);
        }
    }

    public function disposeVisualOnly():void {
        graphics.clear();
        if (parent) parent.removeChild(this);
    }
}
