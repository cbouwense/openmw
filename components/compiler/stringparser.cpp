#include "stringparser.hpp"

#include <algorithm>
#include <iterator>

#include <components/misc/strings/lower.hpp>

#include "scanner.hpp"
#include "generator.hpp"
#include "context.hpp"
#include "extensions.hpp"

namespace Compiler
{
    StringParser::StringParser (ErrorHandler& errorHandler, const Context& context, Literals& literals)
    : Parser (errorHandler, context), mLiterals (literals), mSmashCase (false), mDiscard (false)
    {

    }

    bool StringParser::parseName (const std::string& name, const TokenLoc& loc,
        Scanner& scanner)
    {
        start();
        mTokenLoc = loc;

        if (!mDiscard)
        {
            if (mSmashCase)
                Generator::pushString (mCode, mLiterals, Misc::StringUtils::lowerCase (name));
            else
                Generator::pushString (mCode, mLiterals, name);
        }

        return false;
    }

    bool StringParser::parseKeyword (int keyword, const TokenLoc& loc, Scanner& scanner)
    {
        if (const Extensions *extensions = getContext().getExtensions())
        {
            std::string argumentType; // ignored
            bool hasExplicit = false; // ignored
            if (extensions->isInstruction (keyword, argumentType, hasExplicit))
            {
                // pretend this is not a keyword
                std::string name = loc.mLiteral;
                if (name.size()>=2 && name[0]=='"' && name[name.size()-1]=='"')
                    name = name.substr (1, name.size()-2);
                return parseName (name, loc, scanner);
            }
        }

        if (keyword==Scanner::K_end || keyword==Scanner::K_begin ||
            keyword==Scanner::K_short || keyword==Scanner::K_long ||
            keyword==Scanner::K_float || keyword==Scanner::K_if ||
            keyword==Scanner::K_endif || keyword==Scanner::K_else ||
            keyword==Scanner::K_elseif || keyword==Scanner::K_while ||
            keyword==Scanner::K_endwhile || keyword==Scanner::K_return ||
            keyword==Scanner::K_messagebox || keyword==Scanner::K_set ||
            keyword==Scanner::K_to)
        {
            // pretend this is not a keyword
            std::string name = loc.mLiteral;
            if (name.size()>=2 && name[0]=='"' && name[name.size()-1]=='"')
                name = name.substr (1, name.size()-2);
            return parseName (name, loc, scanner);
        }

        return Parser::parseKeyword (keyword, loc, scanner);
    }

    bool StringParser::parseInt (int value, const TokenLoc& loc, Scanner& scanner)
    {
        reportWarning("Treating integer argument as a string", loc);
        return parseName(loc.mLiteral, loc, scanner);
    }

    void StringParser::append (std::vector<Interpreter::Type_Code>& code)
    {
        std::copy (mCode.begin(), mCode.end(), std::back_inserter (code));
    }

    void StringParser::reset()
    {
        mCode.clear();
        mSmashCase = false;
        mTokenLoc = TokenLoc();
        mDiscard = false;
        Parser::reset();
    }

    void StringParser::smashCase()
    {
        mSmashCase = true;
    }

    const TokenLoc& StringParser::getTokenLoc() const
    {
        return mTokenLoc;
    }

    void StringParser::discard()
    {
        mDiscard = true;
    }
}
