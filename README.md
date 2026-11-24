# pixel-skt-mt-fix

SKT(SK텔레콤) 망에서 Google Pixel 기기로 VoLTE 전화를 수신할 때 통화를 받자마자 끊기는 문제를 해결하는 KernelSU 모듈입니다.

## 지원 기기

- Google Pixel 10 시리즈
- Google Pixel 11 시리즈

> **참고:** 패치 안정성을 위해 대상 라이브러리(`/system_ext/lib64/libimsstack.so`)의 SHA-256 해시가 검증된 빌드와 정확히 일치할 때만 동작합니다. 현재 `v0.1.0`은 Pixel 11 Pro XL(`kodiak`, `CD1A.261005.003`) 바이너리 기준으로 오프셋이 설정되어 있으며, 해시가 다른 기기나 빌드에서는 아무 작업도 수행하지 않고 대기합니다.

## 지원 OS

- Android 17 QPR2
- **사전 요구사항:**
  - KernelSU 루팅 환경 (`/data/adb/ksu/bin/busybox` 사용)
  - SKT VoLTE 활성화 상태 (예: Pixel IMS 등의 캐리어 설정 오버라이드 적용)

## 설치 방법

1. [`pixel_skt_mt_fix.zip`](./pixel_skt_mt_fix.zip) 파일을 기기에 다운로드합니다.
   - 직접 패키징하려면 저장소 루트에서 다음 파일을 ZIP으로 묶습니다: `module.prop`, `common.sh`, `service.sh`, `action.sh`, `uninstall.sh`
2. **KernelSU Manager** 앱을 열고 **모듈(Modules)** 탭에서 **설치(Install)** 를 선택해 `pixel_skt_mt_fix.zip`을 플래싱합니다.
3. 기기를 재부팅합니다. 부팅 완료(`late_start`) 후 백그라운드 서비스가 `com.android.imsstack` 프로세스에 자동으로 패치를 적용합니다.

### 사용 및 제거

- **실시간 켜기/끄기:** KernelSU Manager의 모듈 **Action** 버튼을 누르면 재부팅 없이 실행 중인 IMS 프로세스에 패치를 적용하거나 원본으로 되돌릴 수 있습니다.
- **모듈 삭제:** KernelSU Manager에서 모듈을 제거하면 `uninstall.sh`가 실행 중인 IMS 프로세스의 코드를 원본 명령어로 복원합니다.
- **로그 확인:** 패치 적용 여부와 상태는 기기의 `/data/adb/modules/pixel_skt_mt_fix/patch.log`에서 확인할 수 있습니다.

## 동작 원리

### 문제 원인

SKT 망에서 `Supported: 100rel` 헤더가 포함된 수신 `INVITE`가 도착하면 Pixel의 IMS 스택(`com.android.imsstack` / `libimsstack.so`)은 항상 reliable `183 Session Progress` 응답에 SDP 응답을 먼저 실어 보냅니다. 이미 협상이 완료되었기 때문에 사용자가 전화를 받을 때 보내는 최종 `200 OK`에는 SDP 본문이 비어 있게(`Content-Length: 0`) 됩니다.

SIP 표준(RFC 3261/3262)상 허용되는 흐름이지만, SKT 호 서버는 SDP가 없는 `200 OK`를 거부하고 수락 직후 `Reason: SIP;cause=200;text="Received reject SDP"`와 함께 `BYE`를 전송해 통화를 종료합니다. 반면 삼성 갤럭시 단말은 기본적으로 일반 `180 Ringing`을 먼저 보내고 `200 OK`에 SDP 응답을 담아 보내기 때문에 정상적으로 통화가 연결됩니다. Pixel에서는 이 분기가 네이티브 코드에 하드코딩되어 있어 캐리어 설정(CarrierConfig)만으로는 변경할 수 없습니다.

### 해결 방식

이 모듈은 디스크의 시스템 파일을 전혀 수정하지 않고, 실행 중인 `com.android.imsstack` 프로세스의 메모리에 `/proc/<pid>/mem`을 통해 AArch64 분기 명령어 **1개만** 패치합니다.

```text
libimsstack.so + 0xa83ac4 (IdleState::OnAttached)
  0xa83ac4: bl  MtcExtensionSet::IsAvailableOnBoth("100rel")
  0xa83ac8: tbz w0, #0, 0xa83bac   (원본: 20 07 00 36)
         -> b   0xa83bac           (패치: 39 00 00 14)
```

- 분기가 항상 수행되도록 바뀌면 `IdleState::OnAttached()`는 reliable `183` 경로 대신 `MtcCallState::OnReadyToAlert()`로 바로 진입합니다.
- 그 결과 Pixel은 SDP 없는 일반 `180 Ringing`으로 벨을 울리고, 사용자가 통화를 수락할 때 `200 OK`에 SDP 응답을 담아 전송하여 SKT 망에서 정상 통화가 이루어집니다.
- `IsAvailableOnBoth` 함수 자체는 건드리지 않고 해당 수신 호출 지점만 변경하므로 발신 통화의 PRACK 등 다른 `100rel` 동작에는 영향을 주지 않습니다.

### 안전 장치 및 유지 메커니즘

- **빌드 검증:** `libimsstack.so`의 SHA-256 해시와 패치 지점의 8바이트를 사전에 검사하며, 쓰기 후 다시 읽어 정상 반영 여부를 확인합니다. OS 업데이트 등으로 바이너리가 바뀌면 패치를 건너뜁니다.
- **프로세스 재시작 대응:** `service.sh`가 백그라운드에서 `com.android.imsstack`의 PID를 감시하여, IMS 프로세스가 재시작되어 새 라이브러리가 로드되더라도 현재 설정 상태(`on`/`off`)에 맞춰 자동으로 패치를 다시 적용합니다.

패킷 캡처 비교, 역공학 분석, 제외된 가설 등 자세한 기술 내용은 [docs/ims-call-flow-analysis.md](./docs/ims-call-flow-analysis.md)를 참고하세요.
