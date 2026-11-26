import BrinewellCore
import Foundation
import XCTest
@testable import BrinewellVMRuntime

final class VMAdminProtocolTests: XCTestCase {
    private let snapshot = """
    {"ssh":{"serviceLoad":"loaded","serviceActive":"active","serviceSub":"running","serviceUnitFile":"enabled","socketLoad":"loaded","socketActive":"inactive","socketSub":"dead","socketUnitFile":"disabled","listeningOn22":true},"password":{"state":"set","rev":2}}
    """

    func testStreamingAndRecovery() throws {
        let hello = "{\"type\":\"hello\",\"version\":1,\"instance\":\"abcd\",\"bootId\":\"boot\",\"seq\":1,\"snapshot\":\(snapshot)}"
        let state = "{\"type\":\"state\",\"instance\":\"abcd\",\"seq\":2,\"snapshot\":\(snapshot)}"
        let reply = "{\"type\":\"reply\",\"instance\":\"abcd\",\"id\":\"r\",\"ok\":false,\"code\":\"unitMasked\",\"snapshot\":\(snapshot)}"
        var decoder = VMAdminLineDecoder()
        let first = Array(hello.utf8)
        XCTAssertTrue(decoder.receive(Array(first.prefix(13))).isEmpty)
        let output = decoder.receive(Array(first.dropFirst(13)) + Array("\n\(state)\ninvalid\n".utf8) + Array(repeating: 65, count: 8193) + Array("\n\(reply)\n".utf8))
        XCTAssertEqual(output.count, 3)
        guard output.count == 3 else { return }
        if case .hello(let version, let instance, let boot, let seq, let value) = output[0] {
            XCTAssertEqual(version, 1); XCTAssertEqual(instance, "abcd"); XCTAssertEqual(boot, "boot")
            XCTAssertEqual(seq, 1); XCTAssertTrue(value.ssh.listeningOn22); XCTAssertEqual(value.password.rev, 2)
        } else { XCTFail("hello") }
        if case .state(let instance, let seq, let value) = output[1] {
            XCTAssertEqual(instance, "abcd"); XCTAssertEqual(seq, 2); XCTAssertEqual(value.ssh.serviceSub, "running")
        } else { XCTFail("state") }
        if case .reply(let instance, let id, let ok, let code, let value) = output[2] {
            XCTAssertEqual(instance, "abcd"); XCTAssertEqual(id, "r"); XCTAssertFalse(ok)
            XCTAssertEqual(code, .unitMasked); XCTAssertEqual(value.password.state, .set)
        } else { XCTFail("reply") }
    }

    func testHeartbeatAndBye() {
        var decoder = VMAdminLineDecoder()
        let frames = "{\"type\":\"heartbeat\",\"instance\":\"abcd\",\"seq\":3}\n{\"type\":\"bye\",\"instance\":\"abcd\"}\n"
        XCTAssertEqual(decoder.receive(Array(frames.utf8)), [.heartbeat(instance: "abcd", seq: 3), .bye(instance: "abcd")])
    }

    func testRequestEncoding() throws {
        func object(_ request: VMAdminHostRequest) throws -> NSDictionary {
            try XCTUnwrap(JSONSerialization.jsonObject(with: JSONEncoder().encode(request)) as? NSDictionary)
        }
        XCTAssertEqual(try object(.getSnapshot(id: "1")), ["type": "getSnapshot", "id": "1"])
        XCTAssertEqual(try object(.setSSH(id: "2", enabled: true, targetInstance: "instance")), ["type": "setSSH", "id": "2", "enabled": true, "targetInstance": "instance"])
        XCTAssertEqual(try object(.setPassword(id: "3", password: "secret", confirmUnlock: false, targetInstance: "instance")),
                       ["type": "setPassword", "id": "3", "password": "secret", "confirmUnlock": false, "targetInstance": "instance"])
    }
}
