#pragma once

// Answers the games on the local network that look for servers (see Structs/LanDiscoveryProtocol.h): polled from the
// server's update, never blocks.
class LanResponder
{
public:
    LanResponder() noexcept;
    ~LanResponder() noexcept;

    TP_NOCOPYMOVE(LanResponder);

    // aDescribe gives the JSON object the reply carries; only called when someone asks
    template <class T> void Poll(const T& aDescribe) noexcept
    {
        String description;
        for (int i = 0; i < 16 && HasProbe(); ++i)
        {
            if (description.empty())
                description = aDescribe();
            Reply(description);
        }
    }

private:
    bool HasProbe() noexcept;
    void Reply(const String& acDescription) noexcept;

    uintptr_t m_socket;
    uint8_t m_sender[32]{}; // sockaddr_in of the last probe
    int m_senderSize{0};
};
