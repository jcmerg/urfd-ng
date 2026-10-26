#include "DeviceIO.h"

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <errno.h>
#ifdef __linux__
#include <linux/serial.h>
#endif
#ifdef __APPLE__
#include <IOKit/serial/ioss.h>	// IOSSIOSPEED, IOSSDATALAT
#endif
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

class PosixDevice : public IDeviceIO {
public:
    PosixDevice(): fd_(-1), read_timeout_tenths_(5) {}
    virtual ~PosixDevice() { Close(); }

    bool Open(const std::string &id, std::string &outDescription) override
    {
        // id expected to be a device node path like /dev/ttyUSB0 or a symlink-resolved path.
        // Open non-blocking, so the open does not wait for carrier detect on an adapter
        // that wires it; blocking mode comes back once CLOCAL is set below.
        fd_ = ::open(id.c_str(), O_RDWR | O_NOCTTY | O_SYNC | O_NONBLOCK);
        if (fd_ < 0) {
            if (EBUSY == errno)
                std::cerr << "PosixDevice: " << id << " is in use by another process (another tcd or dvsitest?)" << std::endl;
            else
                std::cerr << "PosixDevice: open(" << id << ") failed: " << strerror(errno) << std::endl;
            return false;
        }

        // the device is ours alone: a second process that opened it would reset the
        // chip under the running one. TIOCEXCL makes later opens fail (EBUSY), except
        // for root on Linux; the lock catches that one, as every tcd and dvsitest takes it.
        if (flock(fd_, LOCK_EX | LOCK_NB) != 0 && EWOULDBLOCK == errno) {
            std::cerr << "PosixDevice: " << id << " is in use by another process (another tcd or dvsitest?)" << std::endl;
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        if (ioctl(fd_, TIOCEXCL) != 0)
            std::cerr << "PosixDevice: exclusive use of " << id << " failed: " << strerror(errno) << std::endl;

        // configure basic 8N1
        struct termios tty;
        if (tcgetattr(fd_, &tty) != 0) {
            std::cerr << "tcgetattr failed: " << strerror(errno) << std::endl;
            Close();
            return false;
        }

        cfmakeraw(&tty);
        tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
        tty.c_cflag |= CLOCAL | CREAD;
        tty.c_cflag &= ~(PARENB | PARODD);
        tty.c_cflag &= ~CSTOPB;
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_lflag = 0;
        tty.c_oflag = 0;
        tty.c_cc[VMIN]  = 1;
        // VTIME is in tenths of a second. Set a small timeout so blocking reads
        // return if the device doesn't respond, preventing probe hangs.
        tty.c_cc[VTIME] = 5; // 0.5 seconds

        // set default baud to 460800 if supported, otherwise 115200
#ifdef B460800
        cfsetospeed(&tty, B460800);
        cfsetispeed(&tty, B460800);
#else
        cfsetospeed(&tty, B115200);
        cfsetispeed(&tty, B115200);
#endif

        if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
            std::cerr << "tcsetattr failed: " << strerror(errno) << std::endl;
            Close();
            return false;
        }

        const int fl = fcntl(fd_, F_GETFL);
        if (fl < 0 || fcntl(fd_, F_SETFL, fl & ~O_NONBLOCK) < 0) {
            std::cerr << "PosixDevice: could not switch " << id << " to blocking mode: " << strerror(errno) << std::endl;
            Close();
            return false;
        }

        SetLowLatency(id);

        outDescription = id;
        return true;
    }

    void Close() override
    {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    ssize_t Write(const void *buf, size_t len) override
    {
        if (fd_ < 0) return -1;
        return ::write(fd_, buf, len);
    }

    ssize_t Read(void *buf, size_t len) override
    {
        if (fd_ < 0) return -1;
        // If timeout is disabled or zero, perform a blocking read
        if (read_timeout_tenths_ <= 0) {
            return ::read(fd_, buf, len);
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd_, &rfds);
        struct timeval tv;
        tv.tv_sec = read_timeout_tenths_ / 10;
        tv.tv_usec = (read_timeout_tenths_ % 10) * 100000;
        int rv = select(fd_ + 1, &rfds, NULL, NULL, &tv);
        if (rv <= 0) {
            // rv == 0 -> timeout, rv < 0 -> error
            return (rv == 0) ? 0 : -1;
        }
        return ::read(fd_, buf, len);
    }

    bool Purge() override
    {
        if (fd_ < 0) return false;
        return (tcflush(fd_, TCIOFLUSH) == 0);
    }

    bool PurgeInput() override
    {
        if (fd_ < 0) return false;
        return (tcflush(fd_, TCIFLUSH) == 0);
    }

    // not tcdrain(): a chip that holds CTS would block it for ever
    bool Drain(int ms) override
    {
        if (fd_ < 0) return false;
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            int n = 0;
            if (ioctl(fd_, TIOCOUTQ, &n) != 0)
                return false;
            if (n <= 0)
                return true;
            if (std::chrono::steady_clock::now() >= end)
                return false;
            usleep(1000);
        }
    }

    bool SetRTS(bool on) override
    {
        if (fd_ < 0) return false;
        int status;
        if (ioctl(fd_, TIOCMGET, &status) == -1) return false;
        if (on)
            status |= TIOCM_RTS;
        else
            status &= ~TIOCM_RTS;
        return (ioctl(fd_, TIOCMSET, &status) == 0);
    }

    bool SetDTR(bool on) override
    {
        if (fd_ < 0) return false;
        int status;
        if (ioctl(fd_, TIOCMGET, &status) == -1) return false;
        if (on)
            status |= TIOCM_DTR;
        else
            status &= ~TIOCM_DTR;
        return (ioctl(fd_, TIOCMSET, &status) == 0);
    }

    bool SetBaudRate(unsigned long baud) override
    {
        if (fd_ < 0) return false;
        struct termios tty;
        if (tcgetattr(fd_, &tty) != 0) return false;
        speed_t sp = B115200;
        switch (baud) {
#ifdef B921600
            case 921600: sp = B921600; break;
#endif
#ifdef B460800
            case 460800: sp = B460800; break;
#endif
            case 115200: sp = B115200; break;
            case 57600: sp = B57600; break;
            case 38400: sp = B38400; break;
#ifdef __APPLE__
            // the macOS termios has no B460800/B921600: IOSSIOSPEED sets any rate. It
            // has to come after the last tcsetattr(), which would reset it, as it does
            // in CDVDevice::OpenDevice().
            case 460800:
            case 921600:
            {
                speed_t speed = baud;
                return (ioctl(fd_, IOSSIOSPEED, &speed) == 0);
            }
#endif
            default: return false;
        }
        cfsetospeed(&tty, sp);
        cfsetispeed(&tty, sp);
        return (tcsetattr(fd_, TCSANOW, &tty) == 0);
    }

    bool SetReadTimeout(int tenths) override
    {
        if (tenths < 0) tenths = 0;
        if (tenths > 255) tenths = 255;
        read_timeout_tenths_ = tenths;
        return true;
    }

    bool SetDataCharacteristics(int bits, int stopbits, int parity) override
    {
        if (fd_ < 0) return false;
        struct termios tty;
        if (tcgetattr(fd_, &tty) != 0) return false;
        tty.c_cflag &= ~CSIZE;
        if (bits == 8) tty.c_cflag |= CS8;
        else if (bits == 7) tty.c_cflag |= CS7;
        else return false;
        if (stopbits == 2) tty.c_cflag |= CSTOPB; else tty.c_cflag &= ~CSTOPB;
        if (parity == 0) tty.c_cflag &= ~PARENB; else tty.c_cflag |= PARENB;
        return (tcsetattr(fd_, TCSANOW, &tty) == 0);
    }

    // non-zero flow: RTS/CTS hardware handshake, as the AMBE devices use
    bool SetFlowControl(int flow) override
    {
        if (fd_ < 0) return false;
        struct termios tty;
        if (tcgetattr(fd_, &tty) != 0) return false;
        if (flow)
            tty.c_cflag |= CRTSCTS;
        else
            tty.c_cflag &= ~CRTSCTS;
        return (tcsetattr(fd_, TCSANOW, &tty) == 0);
    }

    int BytesAvailable() override
    {
        if (fd_ < 0) return -1;
        int n = 0;
        return (ioctl(fd_, FIONREAD, &n) == 0) ? n : -1;
    }

    int WaitReadable(int ms) override
    {
        if (fd_ < 0) return -1;
        struct pollfd pfd = { fd_, POLLIN, 0 };
        const int rv = poll(&pfd, 1, ms);
        if (rv < 0)
            return (EINTR == errno) ? 0 : -1;
        if (0 == rv)
            return 0;
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
            return -1;	// unplugged or reset
        return 1;
    }

    void Abort() override
    {
        if (fd_ < 0) return;
        // a write blocked by RTS/CTS flow control returns once the output is
        // dropped and the descriptor no longer blocks
        const int fl = fcntl(fd_, F_GETFL);
        if (fl >= 0)
            fcntl(fd_, F_SETFL, fl | O_NONBLOCK);
        tcflush(fd_, TCIOFLUSH);
    }

private:
    // The AMBE packets are small and timed: ftdi_sio holds received bytes for up
    // to its latency timer, 16 ms by default, before passing them on. Ask for
    // low latency (ftdi_sio then uses 1 ms), and set the timer through sysfs too.
    void SetLowLatency(const std::string &id)
    {
#ifdef __APPLE__
        // IOSSDATALAT is documented in microseconds, but Apple's FTDI driver
        // (com.apple.DriverKit-AppleUSBFTDI) writes the value as it is into the FTDI
        // latency timer, in ms and cut to a byte: 1000 gives 1000 & 0xff = 232 ms per
        // answer. 1 gives the 1 ms the Linux code below sets.
        unsigned long latency = 1;
        if (ioctl(fd_, IOSSDATALAT, &latency) != 0)
            std::cerr << "PosixDevice: IOSSDATALAT for " << id << " failed: " << strerror(errno) << std::endl;
#elif defined(__linux__)
        struct serial_struct ser;
        if (ioctl(fd_, TIOCGSERIAL, &ser) == 0) {
            ser.flags |= ASYNC_LOW_LATENCY;
            if (ioctl(fd_, TIOCSSERIAL, &ser) != 0)
                std::cerr << "PosixDevice: low latency request for " << id << " failed: " << strerror(errno) << std::endl;
        }

        char real[PATH_MAX];
        if (nullptr == realpath(id.c_str(), real))
            return;
        std::string tty(real);
        tty = tty.substr(tty.rfind('/') + 1);
        const std::string timer = "/sys/bus/usb-serial/devices/" + tty + "/latency_timer";
        {
            std::ofstream out(timer);
            if (out)
                out << "1";
        }
        std::ifstream in(timer);
        int ms = -1;
        if (in >> ms) {
            if (ms > 1)
                std::cerr << "WARNING: " << tty << " USB latency timer is " << ms << " ms, expect audio timing problems; run tcd as root or set " << timer << " to 1" << std::endl;
            else
                std::cout << tty << " USB latency timer: " << ms << " ms" << std::endl;
        }
#else
        (void)id;
#endif
    }

    int fd_;
    int read_timeout_tenths_;
};

// Factory helper (user can instantiate PosixDevice directly later)

IDeviceIO *CreatePosixDevice()
{
    return new PosixDevice();
}
