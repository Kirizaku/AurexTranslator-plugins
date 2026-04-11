#include <iostream>
#include <string>

#if defined(WIN32)
#include <Windows.h>
#endif

template<std::size_t N>
void print_story(const std::string_view (&lines)[N])
{
    for (std::size_t i = 0; i < N; ++i) {
        std::cout << lines[i] << '\n';
        std::cin.get();
    }
}

enum class Language { English, Russian };

Language choose_language()
{
    while (true) {
        std::cout << "Choose language / Выберите язык:\n"
                  << "   1 – English (eng)\n"
                  << "   2 – Русский (ru)\n"
                  << "Enter number: ";
        std::string inp;
        std::getline(std::cin, inp);
        if (inp == "1") return Language::English;
        if (inp == "2") return Language::Russian;
        std::cout << "Invalid input, try again / Неверный ввод, попробуйте ещё раз\n\n";
    }
}

int main()
{
#if defined(WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    const auto lang = choose_language();
    std::cout << "\n\n\n";

    constexpr std::string_view eng_story[] = {
        "You awaken in a cold, moss‑covered courtyard of an ancient city.",
        "Fog rolls over the stone slabs, and in the distance a distant bell tolls.",
        "As you rise, you notice ruins tangled with vines.",
        "A faded inscription is etched on one wall:\n\"He who finds the key shall open the door of time.\"",
        "You start digging through the rubble.",
        "After a while, an old iron key emerges from the dirt.",
        "When you pick it up, it glows with a soft golden light.",
        "The light bursts outward, forming a swirling vortex that opens a shimmering portal.",
        "You step forward… and find yourself in a new realm filled with ancient libraries and star maps.",
        "\n*** The End ***",
        "\nStory author – duck.ai (gpt-oss 120B)"
    };

    constexpr std::string_view ru_story[] = {
        "Вы просыпаетесь в холодном, покрытом мхом дворе древнего города.",
        "Туман стелется над каменными плитами, а вдалеке слышен отдалённый звон колокола.",
        "Поднимаясь, вы замечаете руины, покрытые виноградными лозами.",
        "На одной стене выбито выцветшее послание:\n\"Тот, кто найдёт ключ, откроет дверь времени.\"",
        "Вы начинаете копаться в развалине.",
        "Через несколько минут в грязи всплывает старый железный ключ.",
        "Когда вы берёте его, он начинает светиться мягким золотым светом.",
        "Свет вырывается наружу, создавая вихрь энергии, который открывает мерцающий портал.",
        "Вы делаете шаг вперёд… и оказываетесь в новом мире, полном древних библиотек и звёздных карт.",
        "\n*** Конец ***",
        "\nАвтор рассказа – duck.ai (gpt-oss 120B)"
    };

    if (lang == Language::English) {
        print_story(eng_story);
        std::cout << "\nPress any key to exit...";
    }
    else {
        print_story(ru_story);
        std::cout << "\nНажмите любую клавишу для выхода…";
    }

    std::cin.get();

    return 0;
}