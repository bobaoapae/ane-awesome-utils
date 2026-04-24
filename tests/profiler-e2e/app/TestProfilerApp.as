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
//   Scenario L:  Memory capture with intentional retained ByteArrays so the
//                analyzer can distinguish a leak-like run from Scenario C.
//                Output: test_capture_L.aneprof
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
    import flash.display.Shape;
    import flash.display.Sprite;
    import flash.events.Event;
    import flash.events.EventDispatcher;
    import flash.filesystem.File;
    import flash.filesystem.FileMode;
    import flash.filesystem.FileStream;
    import flash.geom.Point;
    import flash.utils.ByteArray;
    import flash.utils.Dictionary;
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
            for each (var n:String in ["A", "B", "C", "D", "E", "L"]) {
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
                    { name:"E", label:"hidden-listener-leak", frames:180,
                      timing:true, memory:true, snapshots:true, snapshotIntervalMs:1000,
                      listenerLeak:true },
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
                listenerLeakCreated: 0,
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
                    leak: Boolean(sc.leak) || Boolean(sc.listenerLeak),
                    listenerLeak: Boolean(sc.listenerLeak)
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
            util.profilerSnapshot("pre-stop");

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
                    listenerLeakCreated: active.listenerLeakCreated,
                    markers:         ["battle.start", "battle.tick*", "battle.end"]
                };
                log("[test] scenario " + active.name + " done: " + JSON.stringify(rec.postStop));
                results.push(rec);

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
import flash.events.Event;
import flash.events.EventDispatcher;
import flash.geom.Point;
import flash.utils.ByteArray;

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
