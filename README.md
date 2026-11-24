# pixel-skt-mt-fix

SKT(SK텔레콤) 망에서 Google Pixel 기기로 VoLTE 전화를 수신할 때 통화를 받자마자 끊기는 문제를 해결하는 KernelSU 모듈입니다.

## 지원 기기

- Google Pixel 10 시리즈
- Google Pixel 11 시리즈

> **참고:** `v0.2.0`부터는 고정 오프셋이나 바이너리 SHA-256 해시에 의존하지 않고, 실행 시점에 `libimsstack.so`의 심볼과 명령어 패턴으로 패치 위치를 동적으로 찾습니다. 다만 `com.android.imsstack` + `libimsstack.so` 기반 IMS 스택을 사용하는 기기에서만 동작하며, 실제 통화 검증은 Pixel 11 Pro XL(`kodiak`, `CD1A.261005.003`)에서 수행되었습니다.

## 지원 OS

- Android 17 QPR2 (심볼과 함수 구조가 유지되는 후속 펌웨어 빌드 포함)
- **사전 요구사항:**
  - KernelSU 루팅 환경 (`/data/adb/ksu/bin/busybox` 사용)
  - SKT VoLTE 활성화 상태 (예: Pixel IMS 등의 캐리어 설정 오버라이드 적용)

## 설치 방법

1. [GitHub Releases](https://github.com/kyujin-cho/pixel-skt-mt-fix/releases)에서 `pixel_skt_mt_fix-v*.zip` 파일을 다운로드합니다.
   - **직접 빌드:** Android NDK가 설치된 환경(`ANDROID_NDK_HOME` 또는 `$ANDROID_HOME/ndk`)에서 `./build.sh`를 실행하면 정적 arm64 헬퍼 바이너리(`bin/imspatch`)를 컴파일하고 `pixel_skt_mt_fix-<version>.zip` 모듈 ZIP을 생성합니다.
2. **KernelSU Manager** 앱을 열고 **모듈(Modules)** 탭에서 **설치(Install)** 를 선택해 다운로드(또는 빌드)한 ZIP 파일을 플래싱합니다. (`customize.sh`가 `bin/imspatch` 실행 권한을 자동으로 설정합니다.)
3. 기기를 재부팅합니다. 부팅 완료(`late_start`) 후 백그라운드 워처(`service.sh`)가 `com.android.imsstack` 프로세스에 자동으로 패치를 적용합니다.

### 사용 및 제거

- **실시간 켜기/끄기:** KernelSU Manager의 모듈 **Action** 버튼을 누르면 재부팅 없이 실행 중인 IMS 프로세스에 패치를 적용하거나 원본으로 되돌릴 수 있습니다.
- **모듈 삭제:** KernelSU Manager에서 모듈을 제거하면 `uninstall.sh`가 실행 중인 IMS 프로세스의 코드를 원본 명령어로 복원합니다.
- **로그 확인:** 패치 위치, 상태 및 오류 로그는 기기의 `/data/adb/modules/pixel_skt_mt_fix/patch.log`에서 확인할 수 있습니다.

## 동작 원리

### 문제 원인

SKT 망에서 `Supported: 100rel` 헤더가 포함된 수신 `INVITE`가 도착하면 Pixel의 IMS 스택(`com.android.imsstack` / `libimsstack.so`)은 항상 reliable `183 Session Progress` 응답에 SDP 응답을 먼저 실어 보냅니다. 이미 협상이 완료되었기 때문에 사용자가 전화를 받을 때 보내는 최종 `200 OK`에는 SDP 본문이 비어 있게(`Content-Length: 0`) 됩니다.

SIP 표준(RFC 3261/3262)상 허용되는 흐름이지만, SKT 호 서버는 SDP가 없는 `200 OK`를 거부하고 수락 직후 `Reason: SIP;cause=200;text="Received reject SDP"`와 함께 `BYE`를 전송해 통화를 종료합니다. 반면 삼성 갤럭시 단말은 기본적으로 일반 `180 Ringing`을 먼저 보내고 `200 OK`에 SDP 응답을 담아 보내기 때문에 정상적으로 통화가 연결됩니다. Pixel에서는 이 분기가 네이티브 코드에 하드코딩되어 있어 캐리어 설정(CarrierConfig)만으로는 변경할 수 없습니다.

### 해결 방식

이 모듈은 디스크의 시스템 파일을 전혀 수정하지 않고, 정적 네이티브 헬퍼 `bin/imspatch`(`src/imspatch.c`)가 `/proc/<pid>/mem`을 통해 실행 중인 `com.android.imsstack` 프로세스 메모리의 AArch64 분기 명령어 **1개만** 패치합니다.

```text
IdleState::OnAttached() (libimsstack.so)
  bl  MtcExtensionSet::IsAvailableOnBoth("100rel")
  tbz w0, #0, <target>   ->   b <target>
```

- 분기가 항상 수행되도록 바뀌면 `IdleState::OnAttached()`는 reliable `183` 경로 대신 `MtcCallState::OnReadyToAlert()`로 바로 진입합니다.
- 그 결과 Pixel은 SDP 없는 일반 `180 Ringing`으로 벨을 울리고, 사용자가 통화를 수락할 때 `200 OK`에 SDP 응답을 담아 전송하여 SKT 망에서 정상 통화가 이루어집니다.
- `IsAvailableOnBoth` 함수 자체는 건드리지 않고 해당 수신 호출 지점만 변경하므로 발신 통화의 PRACK 등 다른 `100rel` 동작에는 영향을 주지 않습니다.

### 동적 위치 탐색 및 안전 장치

`bin/imspatch`는 패치 지점을 하드코딩하지 않고 런타임에 다음 절차로 탐색·검증합니다:

1. `/proc/<pid>/maps`에서 `libimsstack.so`의 베이스 주소를 얻고, `/proc/<pid>/root`를 통해 매핑된 ELF 파일을 열어 동적 심볼 테이블에서 `IdleState::OnAttached`, `MtcExtensionSet::IsAvailableOnBoth`, `MtcExtensionSet::OPTION_TAG_RPR`을 조회합니다.
2. `IdleState::OnAttached()` 코드에서 `bl` 직후 `tbz w0, #0, <target>` 패턴을 스캔합니다.
3. 라이브 프로세스 메모리의 GOT를 확인해 `bl` 대상(직접 호출 또는 PLT 스텁)이 `IsAvailableOnBoth`의 런타임 주소인지, 그리고 직전 4개 명령어 내의 `adrp`+`ldr x1` 쌍이 `OPTION_TAG_RPR`의 런타임 주소를 로드하는지 검증합니다.
4. 위 조건을 만족하는 지점이 **정확히 1곳**이고, 현재 메모리의 4바이트가 원본 `tbz` 또는 패치된 `b`와 일치할 때만 메모리를 기록하며 쓰기 직후 다시 읽어 확인합니다.

- **펌웨어 업데이트 대응:** 조건 중 하나라도 일치하지 않으면 메모리를 전혀 건드리지 않고 종료합니다. 심볼과 함수 구조가 유지되는 한 OS 업데이트 후에도 별도 오프셋 수정 없이 동작합니다.
- **프로세스 재시작 대응:** `service.sh`가 백그라운드에서 `com.android.imsstack` PID를 감시하다가 프로세스가 재시작되면 새 인스턴스에 현재 상태(`on`/`off`)를 자동으로 재적용합니다.

### 관련 문서

- 패킷 캡처 비교, 역공학 분석 및 제외된 가설: [docs/ims-call-flow-analysis.md](./docs/ims-call-flow-analysis.md)
- 대안 설계 (NFQUEUE 기반 수신 `INVITE` 재작성, 미구현): [docs/design-sip-rewrite.md](./docs/design-sip-rewrite.md)
