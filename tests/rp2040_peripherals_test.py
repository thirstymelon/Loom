#!/usr/bin/env python3
import pathlib
import re
import subprocess
import sys
import tempfile

compiler = pathlib.Path(sys.argv[1]).resolve()
repo_root = pathlib.Path(__file__).resolve().parent.parent
bsp_dir = repo_root / "sdk" / "targets" / "rp2040" / "bsp"

print(f"Testing RP2040 peripherals in {bsp_dir} using {compiler}...")

modules = [
    ("System", """
Module TestSystem
Import System
use System
Block Main Declares
    Rev : Signed_64
    Part : Signed_64
    Asic : Boolean
Implements
    System_Init()
    Rev = Get_Chip_Revision()
    Part = Get_Chip_Part()
    Asic = Is_ASIC_Platform()
    Unreset_Peripherals(RESET_UART0 | RESET_SPI0 | RESET_I2C0)
    Reset_Peripherals(RESET_PWM)
    If Is_Peripheral_Ready(RESET_TIMER)
        System_Reset()
    End If
End Main
"""),
    ("Clock", """
Module TestClock
Import Clock
Import RP2040
use Clock
Block Main Declares
Implements
    RP2040.Check(Clock_Init())
End Main
"""),
    ("GPIO", """
Module TestGPIO
Import GPIO
Import RP2040
use GPIO
Block Run_GPIO_Demo Produces Boolean
Declares
    State : Signed_64
Implements
    If (GPIO_Is_Valid_Pin(25) == False)
        Give False
    End If
    GPIO_Init_Output(25, False)
    GPIO_Write(25, True)
    GPIO_Toggle(25)
    GPIO_Init_Input(24, GPIO_PULL_UP)
    If (GPIO_Read(24) == 1)
        GPIO_Set_Pull(24, GPIO_PULL_DOWN)
    End If
    GPIO_Enable_Interrupt(25, GPIO_IRQ_EDGE_RISE)
    State = GPIO_Interrupt_Status(25)
    GPIO_Clear_Interrupt(25, GPIO_IRQ_EDGE_RISE)
    Give True
End Run_GPIO_Demo
Block Main Implements
    RP2040.Check(Run_GPIO_Demo())
End Main
"""),
    ("Time", """
Module TestTime
Import Time
use Time
Block Main Declares
    Us : Signed_64
    Us64 : Signed_64
    Ms : Signed_64
    Expired : Boolean
Implements
    Us = Get_Time_Microseconds()
    Us64 = Get_Time_64_Microseconds()
    Expired = Timeout_Expired(Us64, 10000)
    Ms = Get_Time()
    Delay(1)
    Delay_Microseconds(100)
    SleepMs(1)
    SleepUs(50)
    Set_Alarm_Target(ALARM_0, 1000000)
    Set_Alarm_Delay(ALARM_1, 50000)
    If Is_Alarm_Armed(ALARM_0)
        Clear_Alarm_Interrupt(ALARM_0)
        Enable_Alarm_Interrupt(ALARM_0)
        Disarm_Alarm(ALARM_0)
    End If
End Main
"""),
    ("UART", """
Module TestUART
Import UART
Import RP2040
use UART
use RP2040
Block Main Declares
    Serial : UART_Handle
    Byte_Value : Signed_64
Implements
    RP2040_Init()
    Serial = UART_Init(0, 1)
    UART_Write(Serial, "Hello from Loom!\\n")
    UART_Write_Byte(Serial, 65)
    If (UART_Byte_Available(Serial))
        Byte_Value = UART_Read_Byte(Serial)
    End If
End Main
"""),
    ("SPI", """
Module SimpleSPI
Import SPI
Import RP2040
use SPI
use RP2040
Block Main Declares
    Bus : SPI_Handle
    Tx : Unsigned_8 Array[2] = {0x06, 0x02}
    Rx : Unsigned_8 Array[2]
Implements
    RP2040_Init()
    Bus = SPI_Init(18, 19, 16)
    SPI_Transfer(Bus, Slice(Tx), Slice(Rx))
End Main
"""),
    ("I2C", """
Module Read_Device
Import RP2040
Import I2C
use RP2040
use I2C
Block Main Declares
    Device_I2C : I2C_Handle
    Value : Signed_64
    Bytes : Unsigned_8 Array[2]
Implements
    RP2040_Init()
    Device_I2C = I2C_Init(4, 5)
    I2C_Write_Register(Device_I2C, 0x68, 0x6B, 0x00)
    Value = I2C_Read_Register(Device_I2C, 0x68, 0x75)
    Bytes[1] = 0x1A
    Bytes[2] = 0x03
    I2C_Write(Device_I2C, 0x68, Slice(Bytes))
End Main
"""),
    ("ADC", """
Module TestADC
Import ADC
Import RP2040
use ADC
Block Read_ADC_Demo Produces Boolean
Declares
    Raw : Signed_64
    Mv : Signed_64
    TempC : Signed_64
    TempMC : Signed_64
Implements
    ADC_Init()
    ADC_Init_Pin(26)
    ADC_Select_Input(ADC_GPIO_26)
    Raw = Read_ADC_Raw(ADC_GPIO_26)
    If (Raw < 0)
        Give False
    End If
    Mv = Read_ADC_Millivolts(ADC_GPIO_26)
    If (Mv < 0)
        Give False
    End If
    TempC = Read_Temperature_Celsius()
    If (TempC == ADC_ERROR)
        Give False
    End If
    TempMC = Read_Temperature_Millicelsius()
    Give (TempMC != ADC_ERROR)
End Read_ADC_Demo
Block Main Implements
    RP2040.Check(Read_ADC_Demo())
End Main
"""),
    ("PWM", """
Module TestPWM
Import PWM
use PWM
Block Main Implements
    PWM_Init(25)
    PWM_Set_Clock_Div(25, 4, 0)
    PWM_Set_Wrap(25, 10000)
    PWM_Set_Enabled(25, True)
    PWM_Set_Mask_Enabled(1)
    Set_PWM_Raw(25, 5000)
    Set_PWM_Duty(25, 75)
End Main
"""),
    ("DMA", """
Module TestDMA
Import DMA
use DMA
Block Main Declares
    Busy : Boolean
    Index : Signed_64
    Base : Signed_64
    Src : Unsigned_32 Array[4]
    Dst : Unsigned_32 Array[4]
Implements
    DMA_Init()
    Index = DMA_Channel_Index(0)
    Base = DMA_Channel_Base(0)
    DMA_Channel_Configure(0, 0x20000000, 0x20000010, 4, DMA_SIZE_32, True, True, DREQ_UNPACED, 0)
    DMA_Channel_Set_Read_Addr(0, 0x20000000)
    DMA_Channel_Set_Write_Addr(0, 0x20000010)
    DMA_Channel_Set_Trans_Count(0, 4)
    DMA_Start_Multi_Channel(1)
    DMA_Channel_Start(0)
    Busy = DMA_Channel_Is_Busy(0)
    DMA_Channel_Wait_Blocking(0)
    DMA_Channel_Abort(0)
    Src[1] = 100
    DMA_Memcpy_32(Cast[Signed_64](Address(Dst[1])), Cast[Signed_64](Address(Src[1])), 4)
End Main
"""),
    ("Watchdog", """
Module TestWatchdog
Import Watchdog
use Watchdog
Block Main Declares
    Caused : Boolean
    Scratch : Unsigned_32
Implements
    Watchdog_Enable(5000, True)
    Watchdog_Feed()
    Caused = Watchdog_Caused_Reboot()
    Watchdog_Set_Scratch(0, 0x12345678)
    Scratch = Watchdog_Get_Scratch(0)
    If (Caused == True)
        Watchdog_Reboot_To_Bootsel()
        Watchdog_Reboot()
    End If
End Main
"""),
    ("Multicore", """
Module TestMulticore
Import Multicore
use Multicore
Block Main Declares
    CoreId : Signed_64
    Locked : Boolean
    Ready : Boolean
    HasData : Boolean
    Msg : Unsigned_32
Implements
    CoreId = Get_Core_Num()
    Locked = Spinlock_Try_Claim(0)
    If (Locked == True)
        Spinlock_Release(0)
    End If
    Spinlock_Claim(1)
    Spinlock_Release(1)
    Ready = Multicore_FIFO_Is_Ready()
    HasData = Multicore_FIFO_Has_Data()
    Multicore_FIFO_Push_Blocking(0x1234)
    Msg = Multicore_FIFO_Pop_Blocking()
    Multicore_FIFO_Drain()
    Multicore_Launch_Core1(0, 0, 0)
End Main
"""),
    ("PIO", """
Module TestPIO
Import PIO
use PIO
Block Main Declares
    Full : Boolean
    Empty : Boolean
    Data : Unsigned_32
    Prog : Unsigned_16 Array[2]
    Base : Signed_64
Implements
    PIO_Init(PIO_0)
    PIO_Init(PIO_1)
    Base = PIO_Base_Address(PIO_0)
    PIO_Load_Instruction(PIO_0, 0, PIO_INSTR_NOP)
    Prog[1] = PIO_INSTR_NOP
    Prog[2] = PIO_INSTR_NOP
    PIO_Load_Program(PIO_0, 0, Slice(Prog))
    PIO_SM_Init(PIO_0, 0, 0, 16, 1)
    Base = PIO_SM_Base(PIO_0, 0)
    PIO_SM_Set_Clock_Div(PIO_0, 0, 4, 0)
    PIO_SM_Set_Enabled(PIO_0, 0, True)
    Full = PIO_SM_Is_TX_Full(PIO_0, 0)
    Empty = PIO_SM_Is_RX_Empty(PIO_0, 0)
    PIO_SM_Put_Blocking(PIO_0, 0, 0x55)
    Data = PIO_SM_Get_Blocking(PIO_0, 0)
End Main
"""),
    ("RTC", """
Module TestRTC
Import RTC
use RTC
Block Main Declares
    Running : Boolean
    Y : Signed_64
    M : Signed_64
    D : Signed_64
    H : Signed_64
    Min : Signed_64
    S : Signed_64
    W : Signed_64
Implements
    RTC_Init()
    RTC_Set_DateTime(2026, 11, 4, 3, 12, 30, 0)
    Running = RTC_Is_Running()
    Y = RTC_Get_Year()
    M = RTC_Get_Month()
    D = RTC_Get_Day()
    H = RTC_Get_Hour()
    Min = RTC_Get_Minute()
    S = RTC_Get_Second()
    W = RTC_Get_Day_Of_Week()
End Main
"""),
    ("Interp", """
Module TestInterp
Import Interp
use Interp
Block Main Declares
    Res : Unsigned_32
    Comb : Unsigned_32
    Base : Signed_64
Implements
    Base = Interp_Base_Address(INTERP_0)
    Interp_Set_Accum(INTERP_0, LANE_0, 0)
    Interp_Add_Accum(INTERP_0, LANE_0, 10)
    Interp_Set_Base(INTERP_0, LANE_0, 100)
    Interp_Set_Base_2(INTERP_0, 200)
    Interp_Set_Base_1And0(INTERP_0, 0x1234, 0x5678)
    Interp_Set_Config(INTERP_0, LANE_0, 0, 0, 31, False, False)
    Res = Interp_Peek_Result(INTERP_0, LANE_0)
    Res = Interp_Pop_Result(INTERP_0, LANE_0)
    Comb = Interp_Peek_Combined(INTERP_0)
    Comb = Interp_Pop_Combined(INTERP_0)
End Main
"""),
    ("RP2040_Umbrella", r"""
Module TestRP2040Umbrella
Import RP2040
use RP2040
Block Main Declares
    Now : Signed_64
    Temp : Signed_64
    Core : Signed_64
    SPI_Bus : SPI_Handle
    I2C_Bus : I2C_Handle
    UART_Bus : UART_Handle
Implements
    RP2040.Check(RP2040_Init())
    RP2040.Check(RP2040_Init_Minimal())
    Core = Get_Core_Num()
    GPIO_Init_Output(25, True)
    UART_Bus = UART_Init(0, 1)
    UART_Write(UART_Bus, "All RP2040 peripherals running!\n")
    SPI_Bus = SPI_Init(18, 19, 16)
    I2C_Bus = I2C_Init(4, 5)
    ADC_Init()
    Temp = Read_Temperature_Celsius()
    PWM_Init(15)
    Set_PWM_Duty(15, 50)
    DMA_Init()
    RTC_Init()
    Now = Get_Time()
    Delay(10)
    GPIO_Write(25, False)
End Main
""")
    ,("SPI_API_COVERAGE", """
Module TestSPIAPI
Import SPI
use SPI
Block Main Declares
    B : Signed_64
    Bus : SPI_Handle
    Tx : Unsigned_8 Array[2]
    Rx : Unsigned_8 Array[2]
Implements
    Bus = SPI_Init(18, 19, 16)
    B = SPI_Transfer_Byte(Bus, 0x9F)
    SPI_Write_Byte(Bus, 0x00)
    B = SPI_Read_Byte(Bus)
    SPI_Write(Bus, Slice(Tx))
    SPI_Read(Bus, Slice(Rx))
    SPI_Transfer(Bus, Slice(Tx), Slice(Rx))
End Main
"""),
    ("I2C_API_COVERAGE", """
Module TestI2CAPI
Import I2C
use I2C
Block Main Declares
    B : Signed_64
    Bus : I2C_Handle
    Bytes : Unsigned_8 Array[2]
Implements
    Bus = I2C_Init(4, 5)
    I2C_Write(Bus, 0x68, Slice(Bytes))
    I2C_Read(Bus, 0x68, Slice(Bytes))
    I2C_Write_Register(Bus, 0x68, 0x6B, 0x00)
    B = I2C_Read_Register(Bus, 0x68, 0x75)
    I2C_Read_Register_Block(Bus, 0x68, 0x3B, Slice(Bytes))
End Main
""")
]

# Every public function declared in the BSP must appear in a tutorial fixture.
all_tutorial_code = "\n".join(code for _, code in modules)
missing_tutorials = []
for module_path in bsp_dir.glob("*.loom"):
    if module_path.stem.endswith("_internal") or not re.fullmatch(r"[A-Za-z0-9_]+", module_path.stem):
        continue
    for function in re.findall(r"^Block\s+(\w+)(?:\s+Takes)?", module_path.read_text(), re.MULTILINE):
        if function != "Global" and not function.endswith(("_Fail", "_Validate_Handle")) and not re.search(r"\b" + re.escape(function) + r"\s*\(", all_tutorial_code):
            missing_tutorials.append(f"{module_path.stem}.{function}")
if missing_tutorials:
    raise SystemExit("missing tutorial calls for public BSP functions: " + ", ".join(missing_tutorials))

fixture_map = {name: code for name, code in modules}
for name in ("ADC",):
    workflow = fixture_map[name]
    if workflow.count("RP2040.Check(") != 1 or "Produces Boolean" not in workflow:
        raise SystemExit(f"{name} legacy walkthrough must return one grouped status and handle it once")
for name in ("UART", "SPI", "I2C"):
    workflow = fixture_map[name]
    handle = {"SPI": "SPI_Handle", "I2C": "I2C_Handle", "UART": "UART_Handle"}[name]
    if handle not in workflow or " = " + name + "_Init(" not in workflow:
        raise SystemExit(f"{name} walkthrough must initialize and use a typed peripheral handle")

for name, code in modules:
    with tempfile.NamedTemporaryFile(suffix=".loom", mode="w", delete=False) as f:
        f.write(code)
        fname = f.name
    res = subprocess.run([str(compiler), "-I", str(bsp_dir), fname], capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[FAIL] {name}: compilation failed!\n{res.stderr}")
        sys.exit(1)
    else:
        print(f"[PASS] {name} peripheral driver compiled cleanly")

print(f"\nAll {len(modules)} RP2040 walkthrough and API-coverage fixtures compiled (no hardware execution).")
